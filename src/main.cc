#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QString>

#include "json.hh"
using json = nlohmann::json;

namespace
{
enum class CmdType
{
    CompileC,
    CompileCpp,
    LinkC,
    LinkCpp,
};

struct compile_cmd
{
    CmdType type;
    double elapsedSeconds;
};

QDir build_dir;
QString profiling_clang_c;
QString profiling_clang_cpp;
std::map<std::string, compile_cmd> compile_cmds;

std::ostream& operator<<(std::ostream& oss, QString s)
{
    return oss << s.toStdString();
}
std::ostream& operator<<(std::ostream& oss, QDir s)
{
    return oss << s.absolutePath().toStdString();
}

bool is_c_compiler(QString cmd)
{
    if (cmd.endsWith("++"))
        return false;

    if (cmd.contains('/'))
        cmd = cmd.mid(cmd.indexOf('/') + 1);
    if (cmd.contains('\\'))
        cmd = cmd.mid(cmd.indexOf('\\') + 1);

    if (cmd.contains("gcc"))
        return true;
    if (cmd.contains("clang"))
        return true;
    if (cmd.contains("zapcc"))
        return true;

    return false;
}
bool is_cxx_compiler(QString cmd)
{
    if (!cmd.endsWith("++"))
        return false;

    if (cmd.contains('/'))
        cmd = cmd.mid(cmd.indexOf('/') + 1);
    if (cmd.contains('\\'))
        cmd = cmd.mid(cmd.indexOf('\\') + 1);

    if (cmd.contains("gcc"))
        return true;
    if (cmd.contains("clang"))
        return true;
    if (cmd.contains("zapcc"))
        return true;

    return false;
}

QString getTimeJsonOfCompileCmd(QString cmd)
{
    auto i = cmd.indexOf(" -o ");
    if (i == -1)
    {
        std::cerr << "Could not find -o in " << cmd << std::endl;
        return "";
    }
    auto s = cmd.mid(i + 4).trimmed();
    i = s.indexOf(' ');
    if (i != -1)
        s = s.left(i);

    return build_dir.absoluteFilePath(s.left(s.size() - 2) + ".json");
}

QString reprFileOfCmd(CmdType type, QString cmd)
{
    switch (type)
    {
    case CmdType::LinkC:
    case CmdType::LinkCpp:
    {
        auto i = cmd.indexOf(" -o ");
        if (i == -1)
        {
            std::cerr << "Could not find -o in " << cmd << std::endl;
            return "";
        }
        auto s = cmd.mid(i + 4).trimmed();
        i = s.indexOf(' ');
        if (i != -1)
            s = s.left(i);

        return s;
    }
    case CmdType::CompileC:
    case CmdType::CompileCpp:
    {
        auto i = cmd.indexOf(" -c ");
        if (i == -1)
        {
            std::cerr << "Could not find -c in " << cmd << std::endl;
            return "";
        }
        auto s = cmd.mid(i + 4).trimmed();
        i = s.indexOf(' ');
        if (i != -1)
            s = s.left(i);

        return s;
    }
    break;
    default:
        std::cerr << "Unknown cmd type " << int(type) << " of " << cmd << std::endl;
        return "";
    }
}

void loadResults()
{
    auto file = build_dir.absoluteFilePath("compile-timer.json");
    compile_cmds.clear();

    std::ifstream f(file.toStdString());
    if (!f.good())
        return;

    json j;
    f >> j;

    for (auto const& kvp : j.items())
    {
        auto const& cj = kvp.value();
        auto& cmd = compile_cmds[kvp.key()];

        cj.at("type").get_to(cmd.type);
        cj.at("elapsedSeconds").get_to(cmd.elapsedSeconds);
    }

    std::cout << " .. loaded " << compile_cmds.size() << " results" << std::endl;
}

void saveResults()
{
    auto file = build_dir.absoluteFilePath("compile-timer.json");

    json j;
    for (auto const& kvp : compile_cmds)
    {
        json cj;
        auto const& cmd = kvp.second;
        cj["type"] = cmd.type;
        cj["elapsedSeconds"] = cmd.elapsedSeconds;
        j[kvp.first] = cj;
    }

    std::ofstream f(file.toStdString());
    f << j.dump(4);
}

void analyzeResults()
{
    // commands
    {
        auto totalElapsed = 0.0;
        std::ofstream csv(build_dir.absoluteFilePath("ct-commands.csv").toStdString());
        csv << "file,type,time (ms)\n";
        for (auto const& kvp : compile_cmds)
        {
            auto f = reprFileOfCmd(kvp.second.type, QString::fromStdString(kvp.first));
            csv << f << ",";
            switch (kvp.second.type)
            {
            case CmdType::LinkC:
                csv << "Link C";
                break;
            case CmdType::LinkCpp:
                csv << "Link C++";
                break;
            case CmdType::CompileC:
                csv << "Compile C";
                break;
            case CmdType::CompileCpp:
                csv << "Compile C++";
                break;
            }
            csv << "," << int(kvp.second.elapsedSeconds * 1000) << "\n";
            totalElapsed += kvp.second.elapsedSeconds;
        }

        std::cout << std::endl;
        std::cout << "Total Build Time: " << totalElapsed << " sec" << std::endl;
    }

    // analyze headers
    {
        struct header_info
        {
            double total_secs = 0;
            double own_secs = 0;
            int count = 0;
        };
        std::map<std::string, header_info> headers;
        std::map<std::string, double> totals_us;

        std::map<std::string, double> header_folders_own_us;

        for (auto const& kvp : compile_cmds)
        {
            if (kvp.second.type != CmdType::CompileC && kvp.second.type != CmdType::CompileCpp)
                continue;

            auto time_json = getTimeJsonOfCompileCmd(QString::fromStdString(kvp.first));

            if (!QFile::exists(time_json))
            {
                std::cerr << "no time trace file found at " << time_json << std::endl;
                std::abort();
            }

            std::ifstream file(time_json.toStdString());
            json j;
            file >> j;

            struct curr_header_info
            {
                double ts;
                double dur;
                curr_header_info* parent;
                std::string parent_name;
            };

            std::map<std::string, curr_header_info> curr_headers;

            for (auto const& e : j.at("traceEvents"))
            {
                if (!e.count("ph"))
                {
                    std::cerr << "unknown event: " << e.dump() << std::endl;
                    continue;
                }

                std::string ph;
                e.at("ph").get_to(ph);

                if (ph == "M")
                    continue; // metadata

                if (ph != "X")
                {
                    std::cerr << "unknown event: " << e.dump() << std::endl;
                    continue;
                }

                double dur;
                double ts;
                std::string name;
                std::string detail;
                e.at("dur").get_to(dur);
                e.at("ts").get_to(ts);
                e.at("name").get_to(name);

                if (e.at("args").count("detail"))
                {
                    e.at("args").at("detail").get_to(detail);
                    detail = QDir(QString::fromStdString(detail)).canonicalPath().toStdString();
                }

                if (QString::fromStdString(name).startsWith("Total "))
                    totals_us[name] += dur;

                if (name == "Source")
                {
                    auto& hi = headers[detail];
                    hi.total_secs += dur / 1e6;
                    hi.own_secs += dur / 1e6;
                    hi.count++;

                    auto qd = QString::fromStdString(detail);

                    if (!qd.endsWith("bits/mathcalls.h") && //
                        !qd.endsWith("X11/keysym.h") &&     //
                        !qd.endsWith("X11/keysymdef.h") &&  //
                        curr_headers.count(detail))
                        std::cerr << "double header " << detail << " in " << time_json << std::endl;

                    curr_headers[detail] = {ts, dur, nullptr, ""};
                }
            }

            for (auto& kvp : curr_headers)
            {
                auto ts = kvp.second.ts;
                auto dur = kvp.second.dur;
                auto mid = ts + dur / 2;

                // reconstructing parents
                for (auto& kvp2 : curr_headers)
                    if (kvp2.second.dur > dur && kvp2.second.ts <= mid && mid <= kvp2.second.ts + kvp2.second.dur)
                    {
                        if (!kvp.second.parent || kvp.second.parent->dur > kvp2.second.dur)
                        {
                            kvp.second.parent = &kvp2.second;
                            kvp.second.parent_name = kvp2.first;
                        }
                    }

                // if (kvp.second.parent)
                //     std::cout << kvp.first << "'s parent is " << kvp.second.parent_name << std::endl;
            }

            // remove own dur from parents
            for (auto const& kvp : curr_headers)
                if (kvp.second.parent)
                    headers[kvp.second.parent_name].own_secs -= kvp.second.dur / 1e6;
        }

        // folders
        for (auto const& kvp : headers)
        {
            auto d = QDir(QString::fromStdString(kvp.first));
            d.cdUp();
            auto f = d.path();
            header_folders_own_us[f.toStdString()] += kvp.second.own_secs;
        }

        // output
        {
            std::ofstream csv(build_dir.absoluteFilePath("ct-headers.csv").toStdString());
            csv << "file,count,own time (ms),total time (ms),avg own time(ms),avg time(ms)\n";
            for (auto const& kvp : headers)
            {
                csv << kvp.first << "," << kvp.second.count;
                csv << "," << kvp.second.own_secs * 1000;
                csv << "," << kvp.second.total_secs * 1000;
                csv << "," << kvp.second.own_secs * 1000 / kvp.second.count;
                csv << "," << kvp.second.total_secs * 1000 / kvp.second.count;
                csv << "\n";
            }
        }
        {
            std::ofstream csv(build_dir.absoluteFilePath("ct-header-folders.csv").toStdString());
            csv << "folder,own time (ms)\n";
            for (auto const& kvp : header_folders_own_us)
            {
                csv << kvp.first << "," << kvp.second * 1000 << "\n";
            }
        }

        // totals
        {
            auto totalSource = 0.0;
            auto totalSourceOwn = 0.0;
            for (auto const& kvp : headers)
            {
                totalSource += kvp.second.total_secs;
                totalSourceOwn += kvp.second.own_secs;
            }

            std::vector<std::pair<double, std::string>> tots;
            for (auto const& kvp : totals_us)
                tots.emplace_back(-kvp.second, kvp.first);
            sort(tots.begin(), tots.end());

            std::cout << std::endl;
            std::cout << totalSource << " sec - Total Header Parsing" << std::endl;
            std::cout << totalSourceOwn << " sec - Total Header Parsing (no transitive includes)" << std::endl;
            std::cout << std::endl;
            for (auto const& kvp : tots)
                std::cout << -kvp.first / 1e6 << " sec - " << kvp.second << std::endl;
        }
    }
}

void process_cmd(QString cmd, CmdType type, QString cmd_args)
{
    if (compile_cmds.count(cmd.toStdString()))
        return; // already processed

    QString exec_cmd;

    switch (type)
    {
    case CmdType::LinkC:
    case CmdType::LinkCpp:
        exec_cmd = cmd;
        break;
    case CmdType::CompileC:
        exec_cmd = profiling_clang_c + " -ftime-trace " + cmd_args;
        break;
    case CmdType::CompileCpp:
        exec_cmd = profiling_clang_cpp + " -ftime-trace " + cmd_args;
        break;
    default:
        std::cerr << "unknown cmd type " << int(type) << " of " << cmd << std::endl;
        break;
    }

    QProcess p;
    p.setWorkingDirectory(build_dir.absolutePath());
    std::cout << "Executing command: " << exec_cmd << std::endl;
    auto start = std::chrono::high_resolution_clock::now();
    p.start(exec_cmd);
    p.waitForFinished();
    auto end = std::chrono::high_resolution_clock::now();
    auto& c = compile_cmds[cmd.toStdString()];
    c.type = type;
    c.elapsedSeconds = std::chrono::duration<double>(end - start).count();
    std::cout << "  .. in " << c.elapsedSeconds * 1000 << " ms" << std::endl << std::endl;

    saveResults();
}

} // namespace

int main(int argc, char const** argv)
{
    if (argc != 3)
    {
        /*
         * e.g.
         * build loc: /../builds/../Qt_5_11_3_Clang_7_Zap/Debug/ (contains build.ninja)
         * clang bin:  /../profiling-llvm/llvm/build/bin/clang++
         */
        std::cout << "usage: ~ directory/containing/build.ninja/ path/to/profiling/clang++" << std::endl;

        return EXIT_SUCCESS;
    }

    auto arg_build_dir = QString(argv[1]);
    auto arg_clang_c = QString(argv[2]);
    auto arg_clang_cpp = arg_clang_c;

    if (arg_clang_c.endsWith("++"))
        arg_clang_c = arg_clang_c.left(arg_clang_c.size() - 2);
    if (!arg_clang_cpp.endsWith("++"))
        arg_clang_c += "++";

    profiling_clang_c = arg_clang_c;
    profiling_clang_cpp = arg_clang_cpp;

    std::cout << "Config:" << std::endl;
    std::cout << "  build path:  " << arg_build_dir << std::endl;
    std::cout << "  clang bin:   " << arg_clang_c << std::endl;
    std::cout << "  clang++ bin: " << arg_clang_cpp << std::endl;

    build_dir = QDir(arg_build_dir);
    if (!QFile::exists(build_dir.absoluteFilePath("build.ninja")))
    {
        std::cerr << "build path " << build_dir << " does not contain a build.ninja" << std::endl;
        return EXIT_FAILURE;
    }

    loadResults();

    QProcess p_build_cmds;
    p_build_cmds.setProgram("ninja");
    p_build_cmds.setArguments({"-t", "commands"});
    p_build_cmds.setWorkingDirectory(build_dir.absolutePath());
    p_build_cmds.start();
    p_build_cmds.waitForFinished();
    auto build_args_raw = QString(p_build_cmds.readAllStandardOutput());
    auto build_args = build_args_raw.split('\n');

    std::cout << build_args.size() << " build commands" << std::endl;

    for (auto build_arg : build_args)
    {
        if (build_arg.isEmpty())
            continue;

        if (build_arg.startsWith(": && "))
            build_arg = build_arg.mid(5);
        if (build_arg.endsWith(" && :"))
            build_arg = build_arg.left(build_arg.size() - 5);

        auto i = build_arg.indexOf(' ');
        if (i == -1)
        {
            std::cerr << "unknown build arg '" << build_arg << "'" << std::endl;
            continue;
        }

        auto cmd = build_arg.left(i);
        auto cmd_args = build_arg.mid(i + 1);

        if (cmd == ":")
        {
            std::cerr << "unrecognized command: " << build_arg << std::endl;
        }
        else if (cmd.endsWith("cmake"))
        {
            // stuff like -E remove or create symlink etc.
            // std::cout << arg << std::endl;
        }
        else if (is_c_compiler(cmd))
        {
            if (cmd_args.contains(" -c "))
                process_cmd(build_arg, CmdType::CompileC, cmd_args);
            else if (cmd_args.contains(" -Wl,"))
                process_cmd(build_arg, CmdType::LinkC, cmd_args);
            else
                std::cerr << "Unknown C compiler cmd: " << build_arg << std::endl << std::endl;
        }
        else if (is_cxx_compiler(cmd))
        {
            if (cmd_args.contains(" -c "))
                process_cmd(build_arg, CmdType::CompileCpp, cmd_args);
            else if (cmd_args.contains(" -Wl,"))
                process_cmd(build_arg, CmdType::LinkCpp, cmd_args);
            else
                std::cerr << "Unknown C compiler cmd: " << build_arg << std::endl << std::endl;
        }
        else
        {
            std::cerr << "unrecognized command: " << build_arg << std::endl;
        }
    }

    analyzeResults();

    // for (auto arg : build_args)
    //     std::cout << arg << std::endl;

    return EXIT_SUCCESS;
}
