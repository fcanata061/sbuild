// sbuild.cpp — Simple source-based package helper for LFS-like systems
// Author: fcanata
// License: MIT
//
// Goals:
//  - No dependency resolution; single-package operations
//  - Fetch via curl/git to ./sources
//  - Extract common archive formats into ./work
//  - Auto-apply patches from https, git, or local paths after extract
//  - Build using recipe (.ini) with phases: preconfig, config, build, install, postinstall
//  - Install to DESTDIR using fakeroot if requested
//  - Optional strip of binaries/libraries
//  - Package from DESTDIR to ./packages (tar.zst/tar.xz)
//  - Remove (undo install) using recorded manifest
//  - Logs, registry, sha256 verification, colored TTY output, spinner
//  - Revdep check: scan installed files for broken shared libs with ldd
//  - Hooks: postremove, postsync
//  - Repo sync: git add/commit/push
//  - Scaffolding: create recipe & dirs for a program
//  - Search & info about recipes
//  - CLI with abbreviations
//
// Build: g++ -std=c++17 -O2 -pthread sbuild.cpp -o sbuild
//
// NOTE: This tool shells out to common userland tools: curl, git, tar, unzip, xz, zstd, patch, sha256sum, ldd, file, strip, fakeroot.
// Ensure they are installed in your environment.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

// =============== Terminal utilities ===============
namespace term {
    static const std::string reset = "\033[0m";
    static const std::string bold = "\033[1m";
    static const std::string dim = "\033[2m";
    static const std::string red = "\033[31m";
    static const std::string green = "\033[32m";
    static const std::string yellow = "\033[33m";
    static const std::string blue = "\033[34m";
    static const std::string magenta = "\033[35m";
    static const std::string cyan = "\033[36m";

    bool is_tty() {
        return isatty(fileno(stdout));
    }

    void println(const std::string &tag, const std::string &msg, const std::string &color) {
        if (is_tty()) std::cout << color << tag << reset << " " << msg << "\n";
        else std::cout << tag << " " << msg << "\n";
    }
    void info(const std::string &msg) { println("[INFO]", msg, blue); }
    void ok(const std::string &msg) { println("[ OK ]", msg, green); }
    void warn(const std::string &msg) { println("[WARN]", msg, yellow); }
    void err(const std::string &msg) { println("[FAIL]", msg, red); }
}

// =============== Spinner ===============
class Spinner {
    std::atomic<bool> running{false};
    std::thread th;
    std::string text;
public:
    void start(const std::string &t) {
        text = t;
        running = true;
        th = std::thread([this]{
            const char frames[] = {'|','/','-','\\'};
            size_t i = 0;
            while (running.load()) {
                if (term::is_tty()) {
                    std::cout << "\r" << term::cyan << "[" << frames[i%4] << "] " << text << term::reset << std::flush;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                i++;
            }
            if (term::is_tty()) std::cout << "\r" << std::string(text.size()+6, ' ') << "\r" << std::flush;
        });
    }
    void stop_ok(const std::string &msg) {
        running = false; if (th.joinable()) th.join();
        term::ok(msg);
    }
    void stop_fail(const std::string &msg) {
        running = false; if (th.joinable()) th.join();
        term::err(msg);
    }
};

// =============== Helpers ===============
static std::string run_cmd(const std::string &cmd, int *exitcode=nullptr) {
    std::array<char, 4096> buf{};
    std::string out;
    FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return "";
    while (fgets(buf.data(), (int)buf.size(), pipe)) out += buf.data();
    int rc = pclose(pipe);
    if (exitcode) *exitcode = WEXITSTATUS(rc);
    return out;
}

static bool run_cmd_checked(const std::string &cmd, const std::string &what, const std::string &logfile) {
    Spinner sp; sp.start(what);
    std::string full = cmd + " >> '" + logfile + "' 2>&1";
    int ec = std::system(full.c_str());
    if (ec == 0) { sp.stop_ok(what + " — done"); return true; }
    sp.stop_fail(what + " — error (code " + std::to_string(WEXITSTATUS(ec)) + ")");
    return false;
}

static std::string sha256_file(const fs::path &p) {
    int ec=0; auto out = run_cmd("sha256sum '" + p.string() + "'", &ec);
    if (ec != 0) return "";
    std::istringstream iss(out);
    std::string hash; iss >> hash; return hash;
}

static bool is_elf(const fs::path &p) {
    int ec=0; auto out = run_cmd("file -b '" + p.string() + "'", &ec);
    if (ec!=0) return false;
    return out.find("ELF ") != std::string::npos;
}

static std::string ts_now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[64]; std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

// =============== Paths & Config ===============
struct Paths {
    fs::path root = fs::current_path();
    fs::path recipes = root/"recipes";
    fs::path sources = root/"sources";
    fs::path work = root/"work";
    fs::path destdir = root/"destdir";
    fs::path packages = root/"packages";
    fs::path logs = root/"logs";
    fs::path registry = root/".sbuild"/"installed";
    fs::path cache = root/".sbuild"/"cache";
    fs::path state = root/".sbuild";
};

static void ensure_dirs(const Paths &P) {
    fs::create_directories(P.recipes);
    fs::create_directories(P.sources);
    fs::create_directories(P.work);
    fs::create_directories(P.destdir);
    fs::create_directories(P.packages);
    fs::create_directories(P.logs);
    fs::create_directories(P.registry);
    fs::create_directories(P.cache);
}

// =============== INI Recipe ===============
struct Recipe {
    std::string name, version, homepage, desc, license;
    std::string source_url; // http(s) URL to tarball/zip
    std::string git_url;    // optional git repo URL
    std::vector<std::string> patches; // http(s), git, or file path
    std::string checksum;   // sha256 of source archive (optional)
    bool opt_strip = false;
    bool opt_fakeroot = true;
    std::string pack_fmt = "zst"; // zst|xz|gz

    // Phases (single shell line; can use && to chain)
    std::string preconfig, config, build, install, postinstall;
    // Hooks
    std::string postremove, postsync;
};

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a==std::string::npos) return ""; return s.substr(a, b-a+1);
}

static bool parse_ini(const fs::path &file, Recipe &r) {
    std::ifstream in(file);
    if (!in) return false;
    std::string sec;
    for (std::string line; std::getline(in,line);) {
        line = trim(line);
        if (line.empty() || line[0]=='#' || line[0]==';') continue;
        if (line.front()=='[' && line.back()==']') { sec = line.substr(1, line.size()-2); continue; }
        auto eq = line.find('='); if (eq==std::string::npos) continue;
        std::string key = trim(line.substr(0,eq));
        std::string val = trim(line.substr(eq+1));
        if (!val.empty() && val.front()=='"' && val.back()=='"') val = val.substr(1,val.size()-2);
        auto put = [&](const std::string &k){ return key==k; };
        if (sec=="package") {
            if (put("name")) r.name = val;
            else if (put("version")) r.version = val;
            else if (put("homepage")) r.homepage = val;
            else if (put("desc")) r.desc = val;
            else if (put("license")) r.license = val;
            else if (put("source")) r.source_url = val;
            else if (put("git")) r.git_url = val;
            else if (put("checksum")) r.checksum = val;
            else if (put("strip")) r.opt_strip = (val=="1"||val=="true"||val=="yes");
            else if (put("fakeroot")) r.opt_fakeroot = !(val=="0"||val=="false"||val=="no");
            else if (put("pack")) r.pack_fmt = val;
            else if (put("patches")) {
                r.patches.clear();
                std::stringstream ss(val);
                for (std::string item; std::getline(ss,item,',');) r.patches.push_back(trim(item));
            }
        } else if (sec=="build") {
            if (put("preconfig")) r.preconfig = val;
            else if (put("config")) r.config = val;
            else if (put("build")) r.build = val;
            else if (put("install")) r.install = val;
            else if (put("postinstall")) r.postinstall = val;
        } else if (sec=="hooks") {
            if (put("postremove")) r.postremove = val;
            else if (put("postsync")) r.postsync = val;
        }
    }
    return !r.name.empty();
}

static void write_recipe_template(const fs::path &file, const std::string &name) {
    std::ofstream o(file);
    o << R"INI(# sbuild recipe (ini)
[package]
name=)INI" << name << R"INI(
version=1.0.0
homepage=https://example.org
license=MIT
desc=Short description.
# Prefer one of: source= (tarball URL) or git=
source=
# git=
# Optional sha256 of source archive (when using source=)
checksum=
# comma-separated list (https://..., git+https://..., file:///path)
patches=
# options
strip=true
fakeroot=true
pack=zst

[build]
# Commands run in extracted source directory. Env: DESTDIR, PREFIX (/usr), JOBS, MAKEFLAGS
preconfig=
config=./configure --prefix=/usr
build=make -j$JOBS
install=make DESTDIR="$DESTDIR" install
postinstall=

[hooks]
postremove=
postsync=
)INI";
}

// =============== Registry and manifests ===============
static fs::path pkg_id_dir(const Paths &P, const Recipe &r) {
    return P.registry / (r.name + "-" + r.version);
}
static fs::path pkg_manifest(const Paths &P, const Recipe &r) {
    return pkg_id_dir(P,r)/"manifest.txt";
}
static fs::path pkg_meta(const Paths &P, const Recipe &r) {
    return pkg_id_dir(P,r)/"meta.ini";
}

static void save_meta(const Paths &P, const Recipe &r) {
    fs::create_directories(pkg_id_dir(P,r));
    std::ofstream m(pkg_meta(P,r));
    m << "name=" << r.name << "\n";
    m << "version=" << r.version << "\n";
    m << "time=" << ts_now() << "\n";
}

static void save_manifest_from_destdir(const Paths &P, const Recipe &r, const fs::path &staging) {
    std::ofstream mf(pkg_manifest(P,r));
    for (auto &p : fs::recursive_directory_iterator(staging)) {
        if (fs::is_regular_file(p.path())) {
            auto rel = fs::relative(p.path(), staging);
            mf << "/" << rel.generic_string() << "\n";
        }
    }
}

// =============== Core operations ===============
static bool fetch_source(const Paths &P, Recipe &r, fs::path &out_srcfile, fs::path &out_srcdir, const std::string &log) {
    ensure_dirs(P);
    if (!r.git_url.empty()) {
        // Clone into sources/name-version
        out_srcdir = P.sources / (r.name + "-" + r.version);
        if (fs::exists(out_srcdir)) {
            term::info("Git source exists, pulling: " + out_srcdir.string());
            return run_cmd_checked("git -C '"+out_srcdir.string()+"' pull --rebase", "git pull", log);
        }
        return run_cmd_checked("git clone '"+r.git_url+"' '"+out_srcdir.string()+"'", "git clone", log);
    }
    if (r.source_url.empty()) { term::err("No source= or git= defined in recipe"); return false; }
    // Download tarball to sources/
    std::string fname = r.name + "-" + r.version;
    std::string ext;
    // Try to guess extension from URL
    auto url = r.source_url;
    auto pos = url.find_last_of('/'); std::string tail = pos==std::string::npos ? url : url.substr(pos+1);
    ext = tail; // full filename
    out_srcfile = P.sources / ext;
    if (fs::exists(out_srcfile)) {
        term::info("Source exists: " + out_srcfile.string());
    } else {
        std::string cmd = "curl -L --fail -o '"+out_srcfile.string()+"' '"+url+"'";
        if (!run_cmd_checked(cmd, "download", log)) return false;
    }
    if (!r.checksum.empty()) {
        auto got = sha256_file(out_srcfile);
        if (got.empty() || got != r.checksum) {
            term::err("sha256 mismatch: got=" + got + " expected=" + r.checksum);
            return false;
        } else term::ok("sha256 verified: " + got);
    }
    return true;
}

static bool extract_source(const Paths &P, const Recipe &r, const fs::path &srcfile, fs::path &out_dir, const std::string &log) {
    if (!srcfile.empty()) {
        // Determine extractor based on extension
        std::string f = srcfile.filename().string();
        out_dir = P.work / (r.name + "-" + r.version);
        fs::remove_all(out_dir);
        fs::create_directories(out_dir);
        std::string cmd;
        if (f.find(".tar.zst")!=std::string::npos) cmd = "tar --zstd -xf '"+srcfile.string()+"' -C '"+out_dir.string()+"' --strip-components=1";
        else if (f.find(".tar.xz")!=std::string::npos) cmd = "tar -xJf '"+srcfile.string()+"' -C '"+out_dir.string()+"' --strip-components=1";
        else if (f.find(".tar.bz2")!=std::string::npos) cmd = "tar -xjf '"+srcfile.string()+"' -C '"+out_dir.string()+"' --strip-components=1";
        else if (f.find(".tar.gz")!=std::string::npos || f.find(".tgz")!=std::string::npos) cmd = "tar -xzf '"+srcfile.string()+"' -C '"+out_dir.string()+"' --strip-components=1";
        else if (f.find(".zip")!=std::string::npos) cmd = "unzip -q '"+srcfile.string()+"' -d '"+out_dir.string()+"' && sh -c 'cd ""'"; // unzip keeps top dir; tolerate
        else { term::err("Unknown archive type: " + f); return false; }
        return run_cmd_checked(cmd, "extract", log);
    } else {
        // git checkout is already a directory
        out_dir = P.sources / (r.name + "-" + r.version);
        if (!fs::exists(out_dir)) { term::err("Git source dir not found: "+out_dir.string()); return false; }
        term::ok("Using git source at " + out_dir.string());
        return true;
    }
}

static bool acquire_patch(const Paths &P, const std::string &p, fs::path &out, const std::string &log) {
    if (p.rfind("git+",0)==0) {
        std::string url = p.substr(4);
        fs::path d = P.cache / ("patch-" + std::to_string(std::hash<std::string>{}(p)));
        if (fs::exists(d)) {
            return run_cmd_checked("git -C '"+d.string()+"' pull --rebase", "patch git pull", log);
        } else {
            fs::create_directories(P.cache);
            return run_cmd_checked("git clone '"+url+"' '"+d.string()+"'", "patch git clone", log) && (out=d/".git");
        }
    } else if (p.rfind("http://",0)==0 || p.rfind("https://",0)==0) {
        fs::path f = P.cache / ("patch-" + std::to_string(std::hash<std::string>{}(p)) + ".patch");
        if (!fs::exists(f)) {
            if (!run_cmd_checked("curl -L --fail -o '"+f.string()+"' '"+p+"'", "download patch", log)) return false;
        }
        out = f; return true;
    } else if (p.rfind("file://",0)==0) {
        out = fs::path(p.substr(7)); return fs::exists(out);
    } else { // assume local path
        out = fs::path(p); return fs::exists(out);
    }
}

static bool apply_patches(const Paths &P, const Recipe &r, const fs::path &srcdir, const std::string &log) {
    for (auto &p : r.patches) {
        fs::path got;
        if (!acquire_patch(P, p, got, log)) { term::err("Failed to acquire patch: " + p); return false; }
        std::string cmd;
        if (fs::is_directory(got)) {
            cmd = "sh -c 'cd '"+srcdir.string()+"' && for f in $(git -C '"+got.string()+"' ls-files \"*.patch\"); do patch -p1 < \""+ (got/"$f").string() + "\"; done'";
        } else {
            cmd = "sh -c 'cd '"+srcdir.string()+"' && patch -p1 < '"+got.string()+"''";
        }
        if (!run_cmd_checked(cmd, "apply patch", log)) return false;
    }
    return true;
}

static bool run_phase(const std::string &phase, const std::string &cmd, const fs::path &cwd, const fs::path &destdir, const Recipe &r, const std::string &log) {
    if (cmd.empty()) { term::info("skip " + phase); return true; }
    std::ostringstream oss;
    oss << "sh -c '";
    oss << "set -e; cd '" << cwd.string() << "'; ";
    oss << "export DESTDIR='"<<destdir.string()<<"'; ";
    oss << "export PREFIX='/usr'; ";
    oss << "export JOBS='"<< std::thread::hardware_concurrency() <<"'; ";
    oss << "export MAKEFLAGS='-j'\"$JOBS\"'; ";
    oss << cmd;
    oss << "'";
    return run_cmd_checked(oss.str(), phase, log);
}

static bool maybe_strip(const fs::path &destdir, const std::string &log) {
    std::string cmd = "sh -c 'set -e; if command -v strip >/dev/null 2>&1; then find '"+destdir.string()+"' -type f -exec sh -c \"file -b \"'{}'\" | grep -q 'ELF' && strip -s \"'{}'\" || true\" \"\\;\" ; fi'";
    return run_cmd_checked(cmd, "strip", log);
}

static bool pack_destdir(const Paths &P, const Recipe &r, const fs::path &destdir, fs::path &out_pkg, const std::string &log) {
    fs::create_directories(P.packages);
    std::string base = r.name + "-" + r.version;
    if (r.pack_fmt=="zst") { out_pkg = P.packages / (base + ".tar.zst"); }
    else if (r.pack_fmt=="xz") { out_pkg = P.packages / (base + ".tar.xz"); }
    else { out_pkg = P.packages / (base + ".tar.gz"); }
    std::string comp = (r.pack_fmt=="zst"?"--zstd": r.pack_fmt=="xz"?"-J":"-z");
    std::string cmd = "tar " + comp + " -C '"+destdir.string()+"' -cf '"+out_pkg.string()+"' .";
    return run_cmd_checked(cmd, "package", log);
}

static bool revdep_check(const fs::path &destdir, const std::string &log) {
    std::string cmd = "sh -c 'set -e; for f in $(find '"+destdir.string()+"' -type f); do if file -b \"$f\" | grep -q ELF; then if ! ldd \"$f\" >/dev/null 2>&1; then echo \"Broken: $f\"; fi; fi; done'";
    return run_cmd_checked(cmd, "revdep", log);
}

// =============== Commands ===============
static int cmd_new(const Paths &P, const std::string &name) {
    ensure_dirs(P);
    fs::path dir = P.recipes/name;
    fs::create_directories(dir);
    fs::path ini = dir/(name+".ini");
    if (!fs::exists(ini)) write_recipe_template(ini, name);
    term::ok("Created recipe scaffold at " + ini.string());
    return 0;
}

static fs::path find_recipe(const Paths &P, const std::string &name) {
    fs::path f1 = P.recipes / name / (name+".ini");
    if (fs::exists(f1)) return f1;
    // fuzzy search
    for (auto &p : fs::recursive_directory_iterator(P.recipes)) {
        if (p.is_regular_file() && p.path().extension()==".ini") {
            if (p.path().filename().string().find(name)!=std::string::npos) return p.path();
        }
    }
    return {};
}

static int cmd_info(const Paths &P, const std::string &name) {
    auto f = find_recipe(P,name);
    if (f.empty()) { term::err("Recipe not found: "+name); return 1; }
    Recipe r; parse_ini(f,r);
    std::cout << term::bold << r.name << term::reset << " " << r.version << "\n";
    std::cout << r.desc << "\n";
    if(!r.homepage.empty()) std::cout << "homepage: " << r.homepage << "\n";
    if(!r.source_url.empty()) std::cout << "source: " << r.source_url << "\n";
    if(!r.git_url.empty()) std::cout << "git:    " << r.git_url << "\n";
    std::cout << "strip:  " << (r.opt_strip?"yes":"no") << ", fakeroot: " << (r.opt_fakeroot?"yes":"no") << ", pack: " << r.pack_fmt << "\n";
    return 0;
}

static int cmd_search(const Paths &P, const std::string &q) {
    int n=0;
    for (auto &p : fs::recursive_directory_iterator(P.recipes)) {
        if (p.is_regular_file() && p.path().extension()==".ini") {
            std::string fn = p.path().filename().string();
            if (fn.find(q)!=std::string::npos) {
                std::cout << fn << "\n"; n++;
            }
        }
    }
    if (n==0) term::warn("No matches.");
    return 0;
}

static int cmd_fetch_extract_patch(const Paths &P, const std::string &name) {
    auto f = find_recipe(P,name);
    if (f.empty()) { term::err("Recipe not found: "+name); return 1; }
    Recipe r; if (!parse_ini(f,r)) { term::err("Invalid recipe."); return 1; }
    fs::create_directories(P.logs);
    fs::path logfile = P.logs / (r.name + "-" + r.version + ".log");
    fs::path srcfile, srcdir, workdir;
    if (!fetch_source(P,r,srcfile,srcdir,logfile.string())) return 2;
    if (!extract_source(P,r,srcfile,workdir,logfile.string())) return 3;
    if (!apply_patches(P,r,workdir,logfile.string())) return 4;
    term::ok("fetch+extract+patch complete: " + workdir.string());
    return 0;
}

static int cmd_build_install(const Paths &P, const std::string &name, bool do_strip, bool do_revdep) {
    auto f = find_recipe(P,name);
    if (f.empty()) { term::err("Recipe not found: "+name); return 1; }
    Recipe r; if (!parse_ini(f,r)) { term::err("Invalid recipe."); return 1; }
    fs::path logfile = P.logs / (r.name + "-" + r.version + ".log");
    fs::path srcfile, srcdir, workdir;
    if (!fetch_source(P,r,srcfile,srcdir,logfile.string())) return 2;
    if (!extract_source(P,r,srcfile,workdir,logfile.string())) return 3;
    if (!apply_patches(P,r,workdir,logfile.string())) return 4;

    fs::path staging = P.destdir / (r.name + "-" + r.version);
    fs::remove_all(staging); fs::create_directories(staging);

    if (!run_phase("preconfig", r.preconfig, workdir, staging, r, logfile.string())) return 5;
    if (!run_phase("config", r.config, workdir, staging, r, logfile.string())) return 6;
    if (!run_phase("build", r.build, workdir, staging, r, logfile.string())) return 7;

    // Install (optionally under fakeroot)
    {
        std::string phase = "install";
        std::ostringstream oss;
        oss << (r.opt_fakeroot?"fakeroot ":"");
        oss << "sh -c 'set -e; cd '"<<workdir.string()<<"'; export DESTDIR='"<<staging.string()<<"'; export PREFIX=/usr; export JOBS='"<< std::thread::hardware_concurrency() <<"'; export MAKEFLAGS='-j'\"$JOBS\"'; ";
        if (!r.install.empty()) oss << r.install; else oss << "make DESTDIR=\"$DESTDIR\" install";
        oss << "'";
        if (!run_cmd_checked(oss.str(), phase, logfile.string())) return 8;
    }

    if (!r.postinstall.empty()) if (!run_phase("postinstall", r.postinstall, workdir, staging, r, logfile.string())) return 9;

    if (do_strip || r.opt_strip) if (!maybe_strip(staging, logfile.string())) return 10;

    // Save registry manifest
    save_meta(P,r);
    save_manifest_from_destdir(P,r,staging);

    if (do_revdep) if (!revdep_check(staging, logfile.string())) term::warn("revdep found issues (see log)");

    term::ok("Installed to DESTDIR: " + staging.string());
    return 0;
}

static int cmd_package(const Paths &P, const std::string &name) {
    auto f = find_recipe(P,name);
    if (f.empty()) { term::err("Recipe not found: "+name); return 1; }
    Recipe r; if (!parse_ini(f,r)) { term::err("Invalid recipe."); return 1; }
    fs::path staging = P.destdir / (r.name + "-" + r.version);
    if (!fs::exists(staging)) { term::err("Nothing to package — build/install first"); return 2; }
    fs::path logfile = P.logs / (r.name + "-" + r.version + ".log");
    fs::path out;
    if (!pack_destdir(P,r,staging,out,logfile.string())) return 3;
    term::ok("Package: " + out.string());
    return 0;
}

static int cmd_remove(const Paths &P, const std::string &name) {
    // Remove from DESTDIR using manifest
    // We accept name or name-version
    std::string id = name;
    fs::path pkgdir;
    for (auto &p : fs::directory_iterator(P.registry)) {
        if (p.is_directory()) {
            if (p.path().filename()==id || p.path().filename().string().rfind(name+"-",0)==0) { pkgdir = p.path(); break; }
        }
    }
    if (pkgdir.empty()) { term::err("No registry entry for: "+name); return 1; }
    std::ifstream mf(pkgdir/"manifest.txt");
    if (!mf) { term::err("Manifest missing for: "+name); return 2; }
    std::string pkgname = pkgdir.filename().string();
    fs::path staging = P.destdir / pkgname;
    int removed=0;
    for (std::string line; std::getline(mf,line);) {
        fs::path f = staging / fs::path(line).relative_path();
        std::error_code ec; if (fs::exists(f,ec)) { fs::remove(f,ec); removed++; }
    }
    fs::remove_all(staging);
    term::ok("Removed files from DESTDIR for " + pkgname + ": " + std::to_string(removed));

    // hook
    std::ifstream meta(pkgdir/"meta.ini");
    Recipe r; r.name=pkgname; // minimal
    // If a recipe exists, try to read hooks
    auto f = find_recipe(P, pkgname.substr(0,pkgname.find_last_of('-')));
    if (!f.empty()) parse_ini(f,r);
    if (!r.postremove.empty()) {
        fs::path logfile = P.logs / (pkgname + ".log");
        run_phase("postremove", r.postremove, fs::current_path(), staging, r, logfile.string());
    }

    fs::remove_all(pkgdir);
    return 0;
}

static int cmd_sync(const Paths &P, const std::string &msg) {
    fs::path logfile = P.logs/"sync.log";
    std::string cmd = "sh -c 'git add -A && git commit -m " + std::string("\"") + (msg.empty()?"sbuild sync":msg) + "\" || true; git push'";
    if (!run_cmd_checked(cmd, "git sync", logfile.string())) return 1;
    // postsync hook (global)
    // (Optional) Users can define a postsync command in .sbuild/hooks.ini
    fs::path hook = P.state/"hooks.ini";
    if (fs::exists(hook)) {
        Recipe r; parse_ini(hook,r); if (!r.postsync.empty()) {
            run_phase("postsync", r.postsync, fs::current_path(), P.destdir, r, logfile.string());
        }
    }
    return 0;
}

static void usage() {
    std::cout << term::bold << "sbuild" << term::reset << " — simples helper de build (LFS)\n\n";
    std::cout << "Uso: sbuild <comando> [args]\n\n";
    std::cout << "Comandos principais (abreviações entre parênteses):\n";
    std::cout << "  new <nome>           (ns)  Criar pasta/receita inicial em recipes/<nome>/<nome>.ini\n";
    std::cout << "  info <nome>                Info da receita\n";
    std::cout << "  search <termo>       (srch)Buscar receitas pelo nome\n";
    std::cout << "  fetch <nome>         (dl)  Baixar fonte (curl/git)\n";
    std::cout << "  extract <nome>       (ex)  Extrair fonte para work/\n";
    std::cout << "  patch <nome>         (pt)  Aplicar patches\n";
    std::cout << "  build <nome>         (b)   Executar preconfig, config, build\n";
    std::cout << "  install <nome>       (i)   Instalar em DESTDIR (fakeroot opcional)\n";
    std::cout << "  bi <nome>                  build+install+patch em um passo (recomendado)\n";
    std::cout << "  package <nome>       (pkg) Empacotar DESTDIR -> packages/*.tar.{zst,xz,gz}\n";
    std::cout << "  remove <nome>        (rm)  Desfazer instalação em DESTDIR com manifest\n";
    std::cout << "  revdep <nome>              Checar libs quebradas no DESTDIR desse pacote\n";
    std::cout << "  sync [mensagem]            git add/commit/push do repositório atual\n";
    std::cout << "  help                  (h)  Esta ajuda\n\n";
    std::cout << "Opções/variáveis de ambiente úteis:\n";
    std::cout << "  SB_STRIP=1            Força strip após install\n";
    std::cout << "  SB_NODEP=1            (no-op, placeholder)\n";
}

int main(int argc, char **argv) {
    Paths P; ensure_dirs(P);
    if (argc<2) { usage(); return 0; }
    std::string cmd = argv[1];
    auto arg = [&](int i){ return (i<argc)? std::string(argv[i]) : std::string(); };

    // aliases
    if (cmd=="h"||cmd=="--help"||cmd=="help"||cmd=="-h") { usage(); return 0; }
    if (cmd=="ns") cmd = "new";
    if (cmd=="srch") cmd = "search";
    if (cmd=="dl") cmd = "fetch";
    if (cmd=="ex") cmd = "extract"; // extract is performed inside bi anyway
    if (cmd=="pt") cmd = "patch";
    if (cmd=="b") cmd = "build";
    if (cmd=="i") cmd = "install";
    if (cmd=="pkg") cmd = "package";
    if (cmd=="rm") cmd = "remove";

    if (cmd=="new") {
        if (argc<3) { term::err("Falta nome: sbuild new <nome>"); return 1; }
        return cmd_new(P, arg(2));
    }
    else if (cmd=="info") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        return cmd_info(P, arg(2));
    }
    else if (cmd=="search") {
        if (argc<3) { term::err("Falta termo"); return 1; }
        return cmd_search(P, arg(2));
    }
    else if (cmd=="fetch"||cmd=="extract"||cmd=="patch") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        return cmd_fetch_extract_patch(P, arg(2));
    }
    else if (cmd=="bi") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        bool do_strip = std::getenv("SB_STRIP")!=nullptr;
        bool do_revdep = true;
        return cmd_build_install(P, arg(2), do_strip, do_revdep);
    }
    else if (cmd=="build"||cmd=="install") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        bool do_strip = std::getenv("SB_STRIP")!=nullptr;
        bool do_revdep = false;
        return cmd_build_install(P, arg(2), do_strip, do_revdep);
    }
    else if (cmd=="package") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        return cmd_package(P, arg(2));
    }
    else if (cmd=="remove") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        return cmd_remove(P, arg(2));
    }
    else if (cmd=="revdep") {
        if (argc<3) { term::err("Falta nome"); return 1; }
        auto f = find_recipe(P,arg(2)); if (f.empty()) { term::err("Recipe não encontrada"); return 1; }
        Recipe r; parse_ini(f,r);
        fs::path staging = P.destdir / (r.name + "-" + r.version);
        if (!fs::exists(staging)) { term::err("Nada instalado em DESTDIR para este pacote"); return 2; }
        fs::path logfile = P.logs / (r.name + "-" + r.version + ".log");
        return revdep_check(staging, logfile.string()) ? 0 : 1;
    }
    else if (cmd=="sync") {
        std::string msg = argc>=3 ? arg(2) : ""; return cmd_sync(P, msg);
    }
    else {
        term::err("Comando desconhecido: " + cmd);
        usage();
        return 1;
    }
}
