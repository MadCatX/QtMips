// SPDX-License-Identifier: GPL-2.0+
/*******************************************************************************
 * QtMips - MIPS 32-bit Architecture Subset Simulator
 *
 * Implemented to support following courses:
 *
 *   B35APO - Computer Architectures
 *   https://cw.fel.cvut.cz/wiki/courses/b35apo
 *
 *   B4M35PAP - Advanced Computer Architectures
 *   https://cw.fel.cvut.cz/wiki/courses/b4m35pap/start
 *
 * Copyright (c) 2017-2019 Karel Koci<cynerd@email.cz>
 * Copyright (c) 2019      Pavel Pisa <pisa@cmp.felk.cvut.cz>
 *
 * Faculty of Electrical Engineering (http://www.fel.cvut.cz)
 * Czech Technical University        (http://www.cvut.cz/)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 ******************************************************************************/

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTextStream>
#include <cctype>
#include <iostream>
#include <fstream>
#include "tracer.h"
#include "reporter.h"
#include "msgreport.h"
#include "simpleasm.h"

using namespace machine;
using namespace std;

void create_parser(QCommandLineParser &p) {
    p.setApplicationDescription("QtMips CLI machine simulator");
    p.addHelpOption();
    p.addVersionOption();

    p.addPositionalArgument("FILE", "Input ELF executable file or assembler source");

    // p.addOptions({}); available only from Qt 5.4+
    p.addOption({"asm", "Treat provided file argument as assembler source."});
    p.addOption({"pipelined", "Configure CPU to use five stage pipeline."});
    p.addOption({"no-delay-slot", "Disable jump delay slot."});
    p.addOption({{"trace-fetch", "tr-fetch"}, "Trace fetched instruction (for both pipelined and not core)."});
    p.addOption({{"trace-decode", "tr-decode"}, "Trace instruction in decode stage. (only for pipelined core)"});
    p.addOption({{"trace-execute", "tr-execute"}, "Trace instruction in execute stage. (only for pipelined core)"});
    p.addOption({{"trace-memory", "tr-memory"}, "Trace instruction in memory stage. (only for pipelined core)"});
    p.addOption({{"trace-writeback", "tr-writeback"}, "Trace instruction in write back stage. (only for pipelined core)"});
    p.addOption({{"trace-pc", "tr-pc"}, "Print program counter register changes."});
    p.addOption({{"trace-gp", "tr-gp"}, "Print general purpose register changes. You can use * for all registers.", "REG"});
    p.addOption({{"trace-lo", "tr-lo"}, "Print LO register changes."});
    p.addOption({{"trace-hi", "tr-hi"}, "Print HI register changes."});
    p.addOption({{"dump-registers", "d-regs"}, "Dump registers state at program exit."});
    p.addOption({"dump-cache-stats", "Dump cache statistics at program exit."});
    p.addOption({"dump-cycles", "Dump number of CPU cycles till program end."});
    p.addOption({"dump-range", "Dump memory range.", "START,LENGTH,FNAME"});
    p.addOption({"load-range", "Load memory range.", "START,FNAME"});
    p.addOption({"expect-fail", "Expect that program causes CPU trap and fail if it doesn't."});
    p.addOption({"fail-match", "Program should exit with exactly this CPU TRAP. Possible values are I(unsupported Instruction), A(Unsupported ALU operation), O(Overflow/underflow) and J(Unaligned Jump). You can freely combine them. Using this implies expect-fail option.", "TRAP"});
    p.addOption({"d-cache", "Data cache. Format policy,sets,words_in_blocks,associativity where policy is random/lru/lfu", "DCACHE"});
    p.addOption({"i-cache", "Instruction cache. Format policy,sets,words_in_blocks,associativity where policy is random/lru/lfu", "ICACHE"});
    p.addOption({"read-time", "Memory read access time (cycles).", "RTIME"});
    p.addOption({"write-time", "Memory read access time (cycles).", "WTIME"});
    p.addOption({"burst-time", "Memory read access time (cycles).", "BTIME"});
}

void configure_cache(MachineConfigCache &cacheconf, QStringList cachearg, QString which) {
    if (cachearg.size() < 1)
        return;
    cacheconf.set_enabled(true);
    QStringList pieces = cachearg.at(cachearg.size() - 1).split(",");
    if (pieces.size() < 3) {
        std::cerr << "Parameters for " << which.toLocal8Bit().data() << " cache incorrect (correct lru,4,2,2,wb)." << std::endl;
        exit(1);
    }
    if (pieces.at(0).size() < 1) {
        std::cerr << "Policy for " << which.toLocal8Bit().data() << " cache is incorrect." << std::endl;
        exit(1);
    }
    if (!pieces.at(0).at(0).isDigit()) {
        if (pieces.at(0).toLower() == "random")
            cacheconf.set_replacement_policy(MachineConfigCache::RP_RAND);
        else if (pieces.at(0).toLower() == "lru")
            cacheconf.set_replacement_policy(MachineConfigCache::RP_LRU);
        else if (pieces.at(0).toLower() == "lfu")
            cacheconf.set_replacement_policy(MachineConfigCache::RP_LFU);
        else {
            std::cerr << "Policy for " << which.toLocal8Bit().data() << " cache is incorrect." << std::endl;
            exit(1);
        }
        pieces.removeFirst();
    }
    if (pieces.size() < 3) {
        std::cerr << "Parameters for " << which.toLocal8Bit().data() << " cache incorrect (correct lru,4,2,2,wb)." << std::endl;
        exit(1);
    }
    cacheconf.set_sets(pieces.at(0).toLong());
    cacheconf.set_blocks(pieces.at(1).toLong());
    cacheconf.set_associativity(pieces.at(2).toLong());
    if (cacheconf.sets() == 0 || cacheconf.blocks() == 0 || cacheconf.associativity() == 0) {
        std::cerr << "Parameters for " << which.toLocal8Bit().data() << " cache cannot have zero component." << std::endl;
        exit(1);
    }
    if (pieces.size() > 3) {
        if (pieces.at(3).toLower() == "wb")
            cacheconf.set_write_policy(MachineConfigCache::WP_BACK);
        else if (pieces.at(3).toLower() == "wt" || pieces.at(3).toLower() == "wtna")
            cacheconf.set_write_policy(MachineConfigCache::WP_THROUGH_NOALLOC);
        else if (pieces.at(3).toLower() == "wta")
            cacheconf.set_write_policy(MachineConfigCache::WP_THROUGH_ALLOC);
        else {
            std::cerr << "Write policy for " << which.toLocal8Bit().data() << " cache is incorrect (correct wb/wt/wtna/wta)." << std::endl;
            exit(1);
        }
    }
}

void configure_machine(QCommandLineParser &p, MachineConfig &cc) {
    QStringList pa = p.positionalArguments();
    int siz;
    if (pa.size() != 1) {
        std::cerr << "Single ELF file has to be specified" << std::endl;
        exit(1);
    }
    cc.set_elf(pa[0]);

    cc.set_delay_slot(!p.isSet("no-delay-slot"));
    cc.set_pipelined(p.isSet("pipelined"));

    siz = p.values("read-time").size();
    if (siz >= 1)
        cc.set_memory_access_time_read(p.values("read-time").at(siz - 1).toLong());
    siz = p.values("write-time").size();
    if (siz >= 1)
        cc.set_memory_access_time_write(p.values("write-time").at(siz - 1).toLong());
    siz = p.values("burst-time").size();
    if (siz >= 1)
        cc.set_memory_access_time_burst(p.values("burst-time").at(siz - 1).toLong());

    configure_cache(*cc.access_cache_data(), p.values("d-cache"), "data");
    configure_cache(*cc.access_cache_program(), p.values("i-cache"), "instruction");
}

void configure_tracer(QCommandLineParser &p, Tracer &tr) {
    if (p.isSet("trace-fetch"))
        tr.fetch();
    if (p.isSet("pipelined")) { // Following are added only if we have stages
        if (p.isSet("trace-decode"))
            tr.decode();
        if (p.isSet("trace-execute"))
            tr.execute();
        if (p.isSet("trace-memory"))
            tr.memory();
        if (p.isSet("trace-writeback"))
            tr.writeback();
    }

    if (p.isSet("trace-pc"))
        tr.reg_pc();

    QStringList gps = p.values("trace-gp");
    for (int i = 0; i < gps.size(); i++) {
        if (gps[i] == "*") {
            for (int y = 0; y < 32; y++)
                tr.reg_gp(y);
        } else {
            bool res;
            int num = gps[i].toInt(&res);
            if (res && num <= 32) {
                tr.reg_gp(num);
            } else {
                cout << "Unknown register number given for trace-gp: " << gps[i].toStdString() << endl;
                exit(1);
            }
        }
    }

    if (p.isSet("trace-lo"))
        tr.reg_lo();
    if (p.isSet("trace-hi"))
        tr.reg_hi();

    // TODO
}

void configure_reporter(QCommandLineParser &p, Reporter &r, const SymbolTable *symtab) {
    if (p.isSet("dump-registers"))
        r.regs();
    if (p.isSet("dump-cache-stats"))
        r.cache_stats();
    if (p.isSet("dump-cycles"))
        r.cycles();

    QStringList fail = p.values("fail-match");
    for (int i = 0; i < fail.size(); i++) {
        for (int y = 0; y < fail[i].length(); y++) {
            enum Reporter::FailReason reason;
            switch (tolower(fail[i].toStdString()[y])) {
            case 'i':
                reason = Reporter::FR_I;
                break;
            case 'a':
                reason = Reporter::FR_A;
                break;
            case 'o':
                reason = Reporter::FR_O;
                break;
            case 'j':
                reason = Reporter::FR_J;
                break;
            default:
                cout << "Unknown fail condition: " << fail[i].toStdString()[y] << endl;
                exit(1);
            }
            r.expect_fail(reason);
        }
    }
    if (p.isSet("expect-fail") && !p.isSet("fail-match"))
        r.expect_fail(Reporter::FailAny);

    foreach (QString range_arg, p.values("dump-range")) {
        std::uint32_t start;
        std::uint32_t len;
        bool ok1 = true;
        bool ok2 = true;
        QString str;
        int comma1 = range_arg.indexOf(",");
        if (comma1 < 0) {
            cout << "Range start missing" << endl;
            exit(1);
        }
        int comma2 = range_arg.indexOf(",", comma1 + 1);
        if (comma2 < 0) {
            cout << "Range lengt/name missing" << endl;
            exit(1);
        }
        str = range_arg.mid(0, comma1);
        if (str.size() >= 1 && !str.at(0).isDigit() && symtab != nullptr) {
            ok1 = symtab->name_to_value(start, str);
        } else {
            start = str.toULong(&ok1, 0);
        }
        str = range_arg.mid(comma1 + 1, comma2 - comma1 - 1);
        if (str.size() >= 1 && !str.at(0).isDigit() && symtab != nullptr) {
            ok2 = symtab->name_to_value(len, str);
        } else {
            len = str.toULong(&ok2, 0);
        }
        if (!ok1 || !ok2) {
            cout << "Range start/length specification error." << endl;
            exit(1);
        }
        r.add_dump_range(start, len, range_arg.mid(comma2 + 1));
    }

    // TODO
}

void load_ranges(QtMipsMachine &machine, const QStringList &ranges) {
    foreach (QString range_arg, ranges) {
        std::uint32_t start;
        bool ok = true;
        QString str;
        int comma1 = range_arg.indexOf(",");
        if (comma1 < 0) {
            cout << "Range start missing" << endl;
            exit(1);
        }
        str = range_arg.mid(0, comma1);
        if (str.size() >= 1 && !str.at(0).isDigit() && machine.symbol_table() != nullptr) {
            ok = machine.symbol_table()->name_to_value(start, str);
        } else {
            start = str.toULong(&ok, 0);
        }
        if (!ok) {
            cout << "Range start/length specification error." << endl;
            exit(1);
        }
        ifstream in;
        std::uint32_t val;
        std::uint32_t addr = start;
        in.open(range_arg.mid(comma1 + 1).toLocal8Bit().data(), ios::in);
        start = start & ~3;
        for (std::string line; getline(in, line); )
        {
            size_t endpos = line.find_last_not_of(" \t\n");
            size_t startpos = line.find_first_not_of(" \t\n");
            size_t idx;
            if (std::string::npos == endpos)
                continue;
            line = line.substr(0, endpos + 1);
            line = line.substr(startpos);
            val = stoul(line, &idx, 0);
            if (idx != line.size()) {
                cout << "cannot parse load range data." << endl;
                exit(1);
            }
            machine.memory_rw()->write_word(addr, val);
            addr += 4;
        }
        in.close();
    }
}

bool assemble(QtMipsMachine &machine, MsgReport &msgrep, QString filename) {
    SymbolTableDb symtab(machine.symbol_table_rw(true));
    machine::MemoryAccess *mem = machine.physical_address_space_rw();
    if (mem == nullptr) {
        return false;
    }
    machine.cache_sync();
    SimpleAsm sasm;

    sasm.connect(&sasm, SIGNAL(report_message(messagetype::Type,QString,int,int,QString,QString)),
            &msgrep, SLOT(report_message(messagetype::Type,QString,int,int,QString,QString)));

    sasm.setup(mem, &symtab, 0x80020000);

    if (!sasm.process_file(filename))
        return false;

    return sasm.finish();
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("qtmips_cli");
    app.setApplicationVersion("0.7");

    QCommandLineParser p;
    create_parser(p);
    p.process(app);

    bool asm_source = p.isSet("asm");

    MachineConfig cc;
    configure_machine(p, cc);
    QtMipsMachine machine(cc, !asm_source, !asm_source);

    Tracer tr(&machine);
    configure_tracer(p, tr);

    Reporter r(&app, &machine);
    configure_reporter(p, r, machine.symbol_table());

    if (asm_source) {
        MsgReport msgrep(&app);
        if (!assemble(machine, msgrep, p.positionalArguments()[0]))
            exit(1);
    }

    load_ranges(machine, p.values("load-range"));

    machine.play();
    return app.exec();
}
