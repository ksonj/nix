#include "util.hh"
#include "affinity.hh"
#include "sync.hh"
#include "finally.hh"
#include "serialise.hh"

#include <array>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/syscall.h>
#endif

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/resource.h>
#endif


extern char * * environ __attribute__((weak));


namespace nix {

std::optional<std::string> getEnv(const std::string & key)
{
    char * value = getenv(key.c_str());
    if (!value) return {};
    return std::string(value);
}


std::map<std::string, std::string> getEnv()
{
    std::map<std::string, std::string> env;
    for (size_t i = 0; environ[i]; ++i) {
        auto s = environ[i];
        auto eq = strchr(s, '=');
        if (!eq)
            // invalid env, just keep going
            continue;
        env.emplace(std::string(s, eq), std::string(eq + 1));
    }
    return env;
}


void clearEnv()
{
    for (auto & name : getEnv())
        unsetenv(name.first.c_str());
}

void replaceEnv(std::map<std::string, std::string> newEnv)
{
    clearEnv();
    for (auto newEnvVar : newEnv)
    {
        setenv(newEnvVar.first.c_str(), newEnvVar.second.c_str(), 1);
    }
}


Path absPath(Path path, std::optional<Path> dir, bool resolveSymlinks)
{
    if (path[0] != '/') {
        if (!dir) {
#ifdef __GNU__
            /* GNU (aka. GNU/Hurd) doesn't have any limitation on path
               lengths and doesn't define `PATH_MAX'.  */
            char *buf = getcwd(NULL, 0);
            if (buf == NULL)
#else
            char buf[PATH_MAX];
            if (!getcwd(buf, sizeof(buf)))
#endif
                throw SysError("cannot get cwd");
            dir = buf;
#ifdef __GNU__
            free(buf);
#endif
        }
        path = *dir + "/" + path;
    }
    return canonPath(path, resolveSymlinks);
}


Path canonPath(const Path & path, bool resolveSymlinks)
{
    assert(path != "");

    string s;

    if (path[0] != '/')
        throw Error("not an absolute path: '%1%'", path);

    string::const_iterator i = path.begin(), end = path.end();
    string temp;

    /* Count the number of times we follow a symlink and stop at some
       arbitrary (but high) limit to prevent infinite loops. */
    unsigned int followCount = 0, maxFollow = 1024;

    while (1) {

        /* Skip slashes. */
        while (i != end && *i == '/') i++;
        if (i == end) break;

        /* Ignore `.'. */
        if (*i == '.' && (i + 1 == end || i[1] == '/'))
            i++;

        /* If `..', delete the last component. */
        else if (*i == '.' && i + 1 < end && i[1] == '.' &&
            (i + 2 == end || i[2] == '/'))
        {
            if (!s.empty()) s.erase(s.rfind('/'));
            i += 2;
        }

        /* Normal component; copy it. */
        else {
            s += '/';
            while (i != end && *i != '/') s += *i++;

            /* If s points to a symlink, resolve it and continue from there */
            if (resolveSymlinks && isLink(s)) {
                if (++followCount >= maxFollow)
                    throw Error("infinite symlink recursion in path '%1%'", path);
                temp = readLink(s) + string(i, end);
                i = temp.begin();
                end = temp.end();
                if (!temp.empty() && temp[0] == '/') {
                    s.clear();  /* restart for symlinks pointing to absolute path */
                } else {
                    s = dirOf(s);
                    if (s == "/") {  // we don’t want trailing slashes here, which dirOf only produces if s = /
                        s.clear();
                    }
                }
            }
        }
    }

    return s.empty() ? "/" : s;
}


Path dirOf(const Path & path)
{
    Path::size_type pos = path.rfind('/');
    if (pos == string::npos)
        return ".";
    return pos == 0 ? "/" : Path(path, 0, pos);
}


std::string_view baseNameOf(std::string_view path)
{
    if (path.empty())
        return "";

    auto last = path.size() - 1;
    if (path[last] == '/' && last > 0)
        last -= 1;

    auto pos = path.rfind('/', last);
    if (pos == string::npos)
        pos = 0;
    else
        pos += 1;

    return path.substr(pos, last - pos + 1);
}


bool isInDir(const Path & path, const Path & dir)
{
    return path[0] == '/'
        && string(path, 0, dir.size()) == dir
        && path.size() >= dir.size() + 2
        && path[dir.size()] == '/';
}


bool isDirOrInDir(const Path & path, const Path & dir)
{
    return path == dir || isInDir(path, dir);
}


struct stat lstat(const Path & path)
{
    struct stat st;
    if (lstat(path.c_str(), &st))
        throw SysError("getting status of '%1%'", path);
    return st;
}


bool pathExists(const Path & path)
{
    int res;
    struct stat st;
    res = lstat(path.c_str(), &st);
    if (!res) return true;
    if (errno != ENOENT && errno != ENOTDIR)
        throw SysError("getting status of %1%", path);
    return false;
}


Path readLink(const Path & path)
{
    checkInterrupt();
    std::vector<char> buf;
    for (ssize_t bufSize = PATH_MAX/4; true; bufSize += bufSize/2) {
        buf.resize(bufSize);
        ssize_t rlSize = readlink(path.c_str(), buf.data(), bufSize);
        if (rlSize == -1)
            if (errno == EINVAL)
                throw Error("'%1%' is not a symlink", path);
            else
                throw SysError("reading symbolic link '%1%'", path);
        else if (rlSize < bufSize)
            return string(buf.data(), rlSize);
    }
}


bool isLink(const Path & path)
{
    struct stat st = lstat(path);
    return S_ISLNK(st.st_mode);
}


DirEntries readDirectory(DIR *dir, const Path & path)
{
    DirEntries entries;
    entries.reserve(64);

    struct dirent * dirent;
    while (errno = 0, dirent = readdir(dir)) { /* sic */
        checkInterrupt();
        string name = dirent->d_name;
        if (name == "." || name == "..") continue;
        entries.emplace_back(name, dirent->d_ino,
#ifdef HAVE_STRUCT_DIRENT_D_TYPE
            dirent->d_type
#else
            DT_UNKNOWN
#endif
        );
    }
    if (errno) throw SysError("reading directory '%1%'", path);

    return entries;
}

DirEntries readDirectory(const Path & path)
{
    AutoCloseDir dir(opendir(path.c_str()));
    if (!dir) throw SysError("opening directory '%1%'", path);

    return readDirectory(dir.get(), path);
}


unsigned char getFileType(const Path & path)
{
    struct stat st = lstat(path);
    if (S_ISDIR(st.st_mode)) return DT_DIR;
    if (S_ISLNK(st.st_mode)) return DT_LNK;
    if (S_ISREG(st.st_mode)) return DT_REG;
    return DT_UNKNOWN;
}


string readFile(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == -1)
        throw SysError("statting file");

    return drainFD(fd, true, st.st_size);
}


string readFile(const Path & path)
{
    AutoCloseFD fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (!fd)
        throw SysError("opening file '%1%'", path);
    return readFile(fd.get());
}


void readFile(const Path & path, Sink & sink)
{
    AutoCloseFD fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (!fd)
        throw SysError("opening file '%s'", path);
    drainFD(fd.get(), sink);
}


void writeFile(const Path & path, std::string_view s, mode_t mode)
{
    AutoCloseFD fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, mode);
    if (!fd)
        throw SysError("opening file '%1%'", path);
    try {
        writeFull(fd.get(), s);
    } catch (Error & e) {
        e.addTrace({}, "writing file '%1%'", path);
        throw;
    }
}


void writeFile(const Path & path, Source & source, mode_t mode)
{
    AutoCloseFD fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, mode);
    if (!fd)
        throw SysError("opening file '%1%'", path);

    std::vector<char> buf(64 * 1024);

    try {
        while (true) {
            try {
                auto n = source.read(buf.data(), buf.size());
                writeFull(fd.get(), {buf.data(), n});
            } catch (EndOfFile &) { break; }
        }
    } catch (Error & e) {
        e.addTrace({}, "writing file '%1%'", path);
        throw;
    }
}

string readLine(int fd)
{
    string s;
    while (1) {
        checkInterrupt();
        char ch;
        // FIXME: inefficient
        ssize_t rd = read(fd, &ch, 1);
        if (rd == -1) {
            if (errno != EINTR)
                throw SysError("reading a line");
        } else if (rd == 0)
            throw EndOfFile("unexpected EOF reading a line");
        else {
            if (ch == '\n') return s;
            s += ch;
        }
    }
}


void writeLine(int fd, string s)
{
    s += '\n';
    writeFull(fd, s);
}


static void _deletePath(int parentfd, const Path & path, uint64_t & bytesFreed)
{
    checkInterrupt();

    string name(baseNameOf(path));

    struct stat st;
    if (fstatat(parentfd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == -1) {
        if (errno == ENOENT) return;
        throw SysError("getting status of '%1%'", path);
    }

    if (!S_ISDIR(st.st_mode) && st.st_nlink == 1)
        bytesFreed += st.st_size;

    if (S_ISDIR(st.st_mode)) {
        /* Make the directory accessible. */
        const auto PERM_MASK = S_IRUSR | S_IWUSR | S_IXUSR;
        if ((st.st_mode & PERM_MASK) != PERM_MASK) {
            if (fchmodat(parentfd, name.c_str(), st.st_mode | PERM_MASK, 0) == -1)
                throw SysError("chmod '%1%'", path);
        }

        int fd = openat(parentfd, path.c_str(), O_RDONLY);
        if (fd == -1)
            throw SysError("opening directory '%1%'", path);
        AutoCloseDir dir(fdopendir(fd));
        if (!dir)
            throw SysError("opening directory '%1%'", path);
        for (auto & i : readDirectory(dir.get(), path))
            _deletePath(dirfd(dir.get()), path + "/" + i.name, bytesFreed);
    }

    int flags = S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0;
    if (unlinkat(parentfd, name.c_str(), flags) == -1) {
        if (errno == ENOENT) return;
        throw SysError("cannot unlink '%1%'", path);
    }
}

static void _deletePath(const Path & path, uint64_t & bytesFreed)
{
    Path dir = dirOf(path);
    if (dir == "")
        dir = "/";

    AutoCloseFD dirfd{open(dir.c_str(), O_RDONLY)};
    if (!dirfd) {
        if (errno == ENOENT) return;
        throw SysError("opening directory '%1%'", path);
    }

    _deletePath(dirfd.get(), path, bytesFreed);
}


void deletePath(const Path & path)
{
    uint64_t dummy;
    deletePath(path, dummy);
}


void deletePath(const Path & path, uint64_t & bytesFreed)
{
    //Activity act(*logger, lvlDebug, format("recursively deleting path '%1%'") % path);
    bytesFreed = 0;
    _deletePath(path, bytesFreed);
}


static Path tempName(Path tmpRoot, const Path & prefix, bool includePid,
    int & counter)
{
    tmpRoot = canonPath(tmpRoot.empty() ? getEnv("TMPDIR").value_or("/tmp") : tmpRoot, true);
    if (includePid)
        return (format("%1%/%2%-%3%-%4%") % tmpRoot % prefix % getpid() % counter++).str();
    else
        return (format("%1%/%2%-%3%") % tmpRoot % prefix % counter++).str();
}


Path createTempDir(const Path & tmpRoot, const Path & prefix,
    bool includePid, bool useGlobalCounter, mode_t mode)
{
    static int globalCounter = 0;
    int localCounter = 0;
    int & counter(useGlobalCounter ? globalCounter : localCounter);

    while (1) {
        checkInterrupt();
        Path tmpDir = tempName(tmpRoot, prefix, includePid, counter);
        if (mkdir(tmpDir.c_str(), mode) == 0) {
#if __FreeBSD__
            /* Explicitly set the group of the directory.  This is to
               work around around problems caused by BSD's group
               ownership semantics (directories inherit the group of
               the parent).  For instance, the group of /tmp on
               FreeBSD is "wheel", so all directories created in /tmp
               will be owned by "wheel"; but if the user is not in
               "wheel", then "tar" will fail to unpack archives that
               have the setgid bit set on directories. */
            if (chown(tmpDir.c_str(), (uid_t) -1, getegid()) != 0)
                throw SysError("setting group of directory '%1%'", tmpDir);
#endif
            return tmpDir;
        }
        if (errno != EEXIST)
            throw SysError("creating directory '%1%'", tmpDir);
    }
}


std::pair<AutoCloseFD, Path> createTempFile(const Path & prefix)
{
    Path tmpl(getEnv("TMPDIR").value_or("/tmp") + "/" + prefix + ".XXXXXX");
    // Strictly speaking, this is UB, but who cares...
    // FIXME: use O_TMPFILE.
    AutoCloseFD fd(mkstemp((char *) tmpl.c_str()));
    if (!fd)
        throw SysError("creating temporary file '%s'", tmpl);
    return {std::move(fd), tmpl};
}


std::string getUserName()
{
    auto pw = getpwuid(geteuid());
    std::string name = pw ? pw->pw_name : getEnv("USER").value_or("");
    if (name.empty())
        throw Error("cannot figure out user name");
    return name;
}


Path getHome()
{
    static Path homeDir = []()
    {
        auto homeDir = getEnv("HOME");
        if (!homeDir) {
            std::vector<char> buf(16384);
            struct passwd pwbuf;
            struct passwd * pw;
            if (getpwuid_r(geteuid(), &pwbuf, buf.data(), buf.size(), &pw) != 0
                || !pw || !pw->pw_dir || !pw->pw_dir[0])
                throw Error("cannot determine user's home directory");
            homeDir = pw->pw_dir;
        }
        return *homeDir;
    }();
    return homeDir;
}


Path getCacheDir()
{
    auto cacheDir = getEnv("XDG_CACHE_HOME");
    return cacheDir ? *cacheDir : getHome() + "/.cache";
}


Path getConfigDir()
{
    auto configDir = getEnv("XDG_CONFIG_HOME");
    return configDir ? *configDir : getHome() + "/.config";
}

std::vector<Path> getConfigDirs()
{
    Path configHome = getConfigDir();
    string configDirs = getEnv("XDG_CONFIG_DIRS").value_or("");
    std::vector<Path> result = tokenizeString<std::vector<string>>(configDirs, ":");
    result.insert(result.begin(), configHome);
    return result;
}


Path getDataDir()
{
    auto dataDir = getEnv("XDG_DATA_HOME");
    return dataDir ? *dataDir : getHome() + "/.local/share";
}


Paths createDirs(const Path & path)
{
    Paths created;
    if (path == "/") return created;

    struct stat st;
    if (lstat(path.c_str(), &st) == -1) {
        created = createDirs(dirOf(path));
        if (mkdir(path.c_str(), 0777) == -1 && errno != EEXIST)
            throw SysError("creating directory '%1%'", path);
        st = lstat(path);
        created.push_back(path);
    }

    if (S_ISLNK(st.st_mode) && stat(path.c_str(), &st) == -1)
        throw SysError("statting symlink '%1%'", path);

    if (!S_ISDIR(st.st_mode)) throw Error("'%1%' is not a directory", path);

    return created;
}


void createSymlink(const Path & target, const Path & link,
    std::optional<time_t> mtime)
{
    if (symlink(target.c_str(), link.c_str()))
        throw SysError("creating symlink from '%1%' to '%2%'", link, target);
    if (mtime) {
        struct timeval times[2];
        times[0].tv_sec = *mtime;
        times[0].tv_usec = 0;
        times[1].tv_sec = *mtime;
        times[1].tv_usec = 0;
        if (lutimes(link.c_str(), times))
            throw SysError("setting time of symlink '%s'", link);
    }
}


void replaceSymlink(const Path & target, const Path & link,
    std::optional<time_t> mtime)
{
    for (unsigned int n = 0; true; n++) {
        Path tmp = canonPath(fmt("%s/.%d_%s", dirOf(link), n, baseNameOf(link)));

        try {
            createSymlink(target, tmp, mtime);
        } catch (SysError & e) {
            if (e.errNo == EEXIST) continue;
            throw;
        }

        if (rename(tmp.c_str(), link.c_str()) != 0)
            throw SysError("renaming '%1%' to '%2%'", tmp, link);

        break;
    }
}


void readFull(int fd, char * buf, size_t count)
{
    while (count) {
        checkInterrupt();
        ssize_t res = read(fd, buf, count);
        if (res == -1) {
            if (errno == EINTR) continue;
            throw SysError("reading from file");
        }
        if (res == 0) throw EndOfFile("unexpected end-of-file");
        count -= res;
        buf += res;
    }
}


void writeFull(int fd, std::string_view s, bool allowInterrupts)
{
    while (!s.empty()) {
        if (allowInterrupts) checkInterrupt();
        ssize_t res = write(fd, s.data(), s.size());
        if (res == -1 && errno != EINTR)
            throw SysError("writing to file");
        if (res > 0)
            s.remove_prefix(res);
    }
}


string drainFD(int fd, bool block, const size_t reserveSize)
{
    StringSink sink(reserveSize);
    drainFD(fd, sink, block);
    return std::move(*sink.s);
}


void drainFD(int fd, Sink & sink, bool block)
{
    int saved;

    Finally finally([&]() {
        if (!block) {
            if (fcntl(fd, F_SETFL, saved) == -1)
                throw SysError("making file descriptor blocking");
        }
    });

    if (!block) {
        saved = fcntl(fd, F_GETFL);
        if (fcntl(fd, F_SETFL, saved | O_NONBLOCK) == -1)
            throw SysError("making file descriptor non-blocking");
    }

    std::vector<unsigned char> buf(64 * 1024);
    while (1) {
        checkInterrupt();
        ssize_t rd = read(fd, buf.data(), buf.size());
        if (rd == -1) {
            if (!block && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            if (errno != EINTR)
                throw SysError("reading from file");
        }
        else if (rd == 0) break;
        else sink({(char *) buf.data(), (size_t) rd});
    }
}



//////////////////////////////////////////////////////////////////////


AutoDelete::AutoDelete() : del{false} {}

AutoDelete::AutoDelete(const string & p, bool recursive) : path(p)
{
    del = true;
    this->recursive = recursive;
}

AutoDelete::~AutoDelete()
{
    try {
        if (del) {
            if (recursive)
                deletePath(path);
            else {
                if (remove(path.c_str()) == -1)
                    throw SysError("cannot unlink '%1%'", path);
            }
        }
    } catch (...) {
        ignoreException();
    }
}

void AutoDelete::cancel()
{
    del = false;
}

void AutoDelete::reset(const Path & p, bool recursive) {
    path = p;
    this->recursive = recursive;
    del = true;
}



//////////////////////////////////////////////////////////////////////


AutoCloseFD::AutoCloseFD() : fd{-1} {}


AutoCloseFD::AutoCloseFD(int fd) : fd{fd} {}


AutoCloseFD::AutoCloseFD(AutoCloseFD && that) : fd{that.fd}
{
    that.fd = -1;
}


AutoCloseFD & AutoCloseFD::operator =(AutoCloseFD && that)
{
    close();
    fd = that.fd;
    that.fd = -1;
    return *this;
}


AutoCloseFD::~AutoCloseFD()
{
    try {
        close();
    } catch (...) {
        ignoreException();
    }
}


int AutoCloseFD::get() const
{
    return fd;
}


void AutoCloseFD::close()
{
    if (fd != -1) {
        if (::close(fd) == -1)
            /* This should never happen. */
            throw SysError("closing file descriptor %1%", fd);
        fd = -1;
    }
}


AutoCloseFD::operator bool() const
{
    return fd != -1;
}


int AutoCloseFD::release()
{
    int oldFD = fd;
    fd = -1;
    return oldFD;
}


void Pipe::create()
{
    int fds[2];
#if HAVE_PIPE2
    if (pipe2(fds, O_CLOEXEC) != 0) throw SysError("creating pipe");
#else
    if (pipe(fds) != 0) throw SysError("creating pipe");
    closeOnExec(fds[0]);
    closeOnExec(fds[1]);
#endif
    readSide = fds[0];
    writeSide = fds[1];
}


void Pipe::close()
{
    readSide.close();
    writeSide.close();
}


//////////////////////////////////////////////////////////////////////


Pid::Pid()
{
}


Pid::Pid(pid_t pid)
    : pid(pid)
{
}


Pid::~Pid()
{
    if (pid != -1) kill();
}


void Pid::operator =(pid_t pid)
{
    if (this->pid != -1 && this->pid != pid) kill();
    this->pid = pid;
    killSignal = SIGKILL; // reset signal to default
}


Pid::operator pid_t()
{
    return pid;
}


int Pid::kill()
{
    assert(pid != -1);

    debug("killing process %1%", pid);

    /* Send the requested signal to the child.  If it has its own
       process group, send the signal to every process in the child
       process group (which hopefully includes *all* its children). */
    if (::kill(separatePG ? -pid : pid, killSignal) != 0) {
        /* On BSDs, killing a process group will return EPERM if all
           processes in the group are zombies (or something like
           that). So try to detect and ignore that situation. */
#if __FreeBSD__ || __APPLE__
        if (errno != EPERM || ::kill(pid, 0) != 0)
#endif
            logError(SysError("killing process %d", pid).info());
    }

    return wait();
}


int Pid::wait()
{
    assert(pid != -1);
    while (1) {
        int status;
        int res = waitpid(pid, &status, 0);
        if (res == pid) {
            pid = -1;
            return status;
        }
        if (errno != EINTR)
            throw SysError("cannot get exit status of PID %d", pid);
        checkInterrupt();
    }
}


void Pid::setSeparatePG(bool separatePG)
{
    this->separatePG = separatePG;
}


void Pid::setKillSignal(int signal)
{
    this->killSignal = signal;
}


pid_t Pid::release()
{
    pid_t p = pid;
    pid = -1;
    return p;
}


void killUser(uid_t uid)
{
    debug("killing all processes running under uid '%1%'", uid);

    assert(uid != 0); /* just to be safe... */

    /* The system call kill(-1, sig) sends the signal `sig' to all
       users to which the current process can send signals.  So we
       fork a process, switch to uid, and send a mass kill. */

    Pid pid = startProcess([&]() {

        if (setuid(uid) == -1)
            throw SysError("setting uid");

        while (true) {
#ifdef __APPLE__
            /* OSX's kill syscall takes a third parameter that, among
               other things, determines if kill(-1, signo) affects the
               calling process. In the OSX libc, it's set to true,
               which means "follow POSIX", which we don't want here
                 */
            if (syscall(SYS_kill, -1, SIGKILL, false) == 0) break;
#else
            if (kill(-1, SIGKILL) == 0) break;
#endif
            if (errno == ESRCH || errno == EPERM) break; /* no more processes */
            if (errno != EINTR)
                throw SysError("cannot kill processes for uid '%1%'", uid);
        }

        _exit(0);
    });

    int status = pid.wait();
    if (status != 0)
        throw Error("cannot kill processes for uid '%1%': %2%", uid, statusToString(status));

    /* !!! We should really do some check to make sure that there are
       no processes left running under `uid', but there is no portable
       way to do so (I think).  The most reliable way may be `ps -eo
       uid | grep -q $uid'. */
}


//////////////////////////////////////////////////////////////////////


/* Wrapper around vfork to prevent the child process from clobbering
   the caller's stack frame in the parent. */
static pid_t doFork(bool allowVfork, std::function<void()> fun) __attribute__((noinline));
static pid_t doFork(bool allowVfork, std::function<void()> fun)
{
#ifdef __linux__
    pid_t pid = allowVfork ? vfork() : fork();
#else
    pid_t pid = fork();
#endif
    if (pid != 0) return pid;
    fun();
    abort();
}


pid_t startProcess(std::function<void()> fun, const ProcessOptions & options)
{
    auto wrapper = [&]() {
        if (!options.allowVfork)
            logger = makeSimpleLogger();
        try {
#if __linux__
            if (options.dieWithParent && prctl(PR_SET_PDEATHSIG, SIGKILL) == -1)
                throw SysError("setting death signal");
#endif
            restoreAffinity();
            fun();
        } catch (std::exception & e) {
            try {
                std::cerr << options.errorPrefix << e.what() << "\n";
            } catch (...) { }
        } catch (...) { }
        if (options.runExitHandlers)
            exit(1);
        else
            _exit(1);
    };

    pid_t pid = doFork(options.allowVfork, wrapper);
    if (pid == -1) throw SysError("unable to fork");

    return pid;
}


std::vector<char *> stringsToCharPtrs(const Strings & ss)
{
    std::vector<char *> res;
    for (auto & s : ss) res.push_back((char *) s.c_str());
    res.push_back(0);
    return res;
}

string runProgram(Path program, bool searchPath, const Strings & args,
    const std::optional<std::string> & input)
{
    auto res = runProgram(RunOptions {.program = program, .searchPath = searchPath, .args = args, .input = input});

    if (!statusOk(res.first))
        throw ExecError(res.first, fmt("program '%1%' %2%", program, statusToString(res.first)));

    return res.second;
}

// Output = error code + "standard out" output stream
std::pair<int, std::string> runProgram(RunOptions && options)
{
    StringSink sink;
    options.standardOut = &sink;

    int status = 0;

    try {
        runProgram2(options);
    } catch (ExecError & e) {
        status = e.status;
    }

    return {status, std::move(*sink.s)};
}

void runProgram2(const RunOptions & options)
{
    checkInterrupt();

    assert(!(options.standardIn && options.input));

    std::unique_ptr<Source> source_;
    Source * source = options.standardIn;

    if (options.input) {
        source_ = std::make_unique<StringSource>(*options.input);
        source = source_.get();
    }

    /* Create a pipe. */
    Pipe out, in;
    if (options.standardOut) out.create();
    if (source) in.create();

    ProcessOptions processOptions;
    // vfork implies that the environment of the main process and the fork will
    // be shared (technically this is undefined, but in practice that's the
    // case), so we can't use it if we alter the environment
    processOptions.allowVfork = !options.environment;

    /* Fork. */
    Pid pid = startProcess([&]() {
        if (options.environment)
            replaceEnv(*options.environment);
        if (options.standardOut && dup2(out.writeSide.get(), STDOUT_FILENO) == -1)
            throw SysError("dupping stdout");
        if (options.mergeStderrToStdout)
            if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
                throw SysError("cannot dup stdout into stderr");
        if (source && dup2(in.readSide.get(), STDIN_FILENO) == -1)
            throw SysError("dupping stdin");

        if (options.chdir && chdir((*options.chdir).c_str()) == -1)
            throw SysError("chdir failed");
        if (options.gid && setgid(*options.gid) == -1)
            throw SysError("setgid failed");
        /* Drop all other groups if we're setgid. */
        if (options.gid && setgroups(0, 0) == -1)
            throw SysError("setgroups failed");
        if (options.uid && setuid(*options.uid) == -1)
            throw SysError("setuid failed");

        Strings args_(options.args);
        args_.push_front(options.program);

        restoreProcessContext();

        if (options.searchPath)
            execvp(options.program.c_str(), stringsToCharPtrs(args_).data());
            // This allows you to refer to a program with a pathname relative
            // to the PATH variable.
        else
            execv(options.program.c_str(), stringsToCharPtrs(args_).data());

        throw SysError("executing '%1%'", options.program);
    }, processOptions);

    out.writeSide.close();

    std::thread writerThread;

    std::promise<void> promise;

    Finally doJoin([&]() {
        if (writerThread.joinable())
            writerThread.join();
    });


    if (source) {
        in.readSide.close();
        writerThread = std::thread([&]() {
            try {
                std::vector<char> buf(8 * 1024);
                while (true) {
                    size_t n;
                    try {
                        n = source->read(buf.data(), buf.size());
                    } catch (EndOfFile &) {
                        break;
                    }
                    writeFull(in.writeSide.get(), {buf.data(), n});
                }
                promise.set_value();
            } catch (...) {
                promise.set_exception(std::current_exception());
            }
            in.writeSide.close();
        });
    }

    if (options.standardOut)
        drainFD(out.readSide.get(), *options.standardOut);

    /* Wait for the child to finish. */
    int status = pid.wait();

    /* Wait for the writer thread to finish. */
    if (source) promise.get_future().get();

    if (status)
        throw ExecError(status, fmt("program '%1%' %2%", options.program, statusToString(status)));
}


void closeMostFDs(const set<int> & exceptions)
{
#if __linux__
    try {
        for (auto & s : readDirectory("/proc/self/fd")) {
            auto fd = std::stoi(s.name);
            if (!exceptions.count(fd)) {
                debug("closing leaked FD %d", fd);
                close(fd);
            }
        }
        return;
    } catch (SysError &) {
    }
#endif

    int maxFD = 0;
    maxFD = sysconf(_SC_OPEN_MAX);
    for (int fd = 0; fd < maxFD; ++fd)
        if (!exceptions.count(fd))
            close(fd); /* ignore result */
}


void closeOnExec(int fd)
{
    int prev;
    if ((prev = fcntl(fd, F_GETFD, 0)) == -1 ||
        fcntl(fd, F_SETFD, prev | FD_CLOEXEC) == -1)
        throw SysError("setting close-on-exec flag");
}


//////////////////////////////////////////////////////////////////////


bool _isInterrupted = false;

static thread_local bool interruptThrown = false;
thread_local std::function<bool()> interruptCheck;

void setInterruptThrown()
{
    interruptThrown = true;
}

void _interrupted()
{
    /* Block user interrupts while an exception is being handled.
       Throwing an exception while another exception is being handled
       kills the program! */
    if (!interruptThrown && !std::uncaught_exceptions()) {
        interruptThrown = true;
        throw Interrupted("interrupted by the user");
    }
}


//////////////////////////////////////////////////////////////////////


template<class C> C tokenizeString(std::string_view s, const string & separators)
{
    C result;
    string::size_type pos = s.find_first_not_of(separators, 0);
    while (pos != string::npos) {
        string::size_type end = s.find_first_of(separators, pos + 1);
        if (end == string::npos) end = s.size();
        string token(s, pos, end - pos);
        result.insert(result.end(), token);
        pos = s.find_first_not_of(separators, end);
    }
    return result;
}

template Strings tokenizeString(std::string_view s, const string & separators);
template StringSet tokenizeString(std::string_view s, const string & separators);
template vector<string> tokenizeString(std::string_view s, const string & separators);


string chomp(std::string_view s)
{
    size_t i = s.find_last_not_of(" \n\r\t");
    return i == string::npos ? "" : string(s, 0, i + 1);
}


string trim(const string & s, const string & whitespace)
{
    auto i = s.find_first_not_of(whitespace);
    if (i == string::npos) return "";
    auto j = s.find_last_not_of(whitespace);
    return string(s, i, j == string::npos ? j : j - i + 1);
}


string replaceStrings(std::string_view s,
    const std::string & from, const std::string & to)
{
    string res(s);
    if (from.empty()) return res;
    size_t pos = 0;
    while ((pos = res.find(from, pos)) != std::string::npos) {
        res.replace(pos, from.size(), to);
        pos += to.size();
    }
    return res;
}


std::string rewriteStrings(const std::string & _s, const StringMap & rewrites)
{
    auto s = _s;
    for (auto & i : rewrites) {
        if (i.first == i.second) continue;
        size_t j = 0;
        while ((j = s.find(i.first, j)) != string::npos)
            s.replace(j, i.first.size(), i.second);
    }
    return s;
}


string statusToString(int status)
{
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status))
            return (format("failed with exit code %1%") % WEXITSTATUS(status)).str();
        else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
#if HAVE_STRSIGNAL
            const char * description = strsignal(sig);
            return (format("failed due to signal %1% (%2%)") % sig % description).str();
#else
            return (format("failed due to signal %1%") % sig).str();
#endif
        }
        else
            return "died abnormally";
    } else return "succeeded";
}


bool statusOk(int status)
{
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}


bool hasPrefix(std::string_view s, std::string_view prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}


bool hasSuffix(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size()
        && s.substr(s.size() - suffix.size()) == suffix;
}


std::string toLower(const std::string & s)
{
    std::string r(s);
    for (auto & c : r)
        c = std::tolower(c);
    return r;
}


std::string shellEscape(const std::string & s)
{
    std::string r = "'";
    for (auto & i : s)
        if (i == '\'') r += "'\\''"; else r += i;
    r += '\'';
    return r;
}


void ignoreException()
{
    try {
        throw;
    } catch (std::exception & e) {
        printError("error (ignored): %1%", e.what());
    }
}

bool shouldANSI()
{
    return isatty(STDERR_FILENO)
        && getEnv("TERM").value_or("dumb") != "dumb"
        && !getEnv("NO_COLOR").has_value();
}

std::string filterANSIEscapes(const std::string & s, bool filterAll, unsigned int width)
{
    std::string t, e;
    size_t w = 0;
    auto i = s.begin();

    while (w < (size_t) width && i != s.end()) {

        if (*i == '\e') {
            std::string e;
            e += *i++;
            char last = 0;

            if (i != s.end() && *i == '[') {
                e += *i++;
                // eat parameter bytes
                while (i != s.end() && *i >= 0x30 && *i <= 0x3f) e += *i++;
                // eat intermediate bytes
                while (i != s.end() && *i >= 0x20 && *i <= 0x2f) e += *i++;
                // eat final byte
                if (i != s.end() && *i >= 0x40 && *i <= 0x7e) e += last = *i++;
            } else {
                if (i != s.end() && *i >= 0x40 && *i <= 0x5f) e += *i++;
            }

            if (!filterAll && last == 'm')
                t += e;
        }

        else if (*i == '\t') {
            i++; t += ' '; w++;
            while (w < (size_t) width && w % 8) {
                t += ' '; w++;
            }
        }

        else if (*i == '\r')
            // do nothing for now
            i++;

        else {
            w++;
            // Copy one UTF-8 character.
            if ((*i & 0xe0) == 0xc0) {
                t += *i++;
                if (i != s.end() && ((*i & 0xc0) == 0x80)) t += *i++;
            } else if ((*i & 0xf0) == 0xe0) {
                t += *i++;
                if (i != s.end() && ((*i & 0xc0) == 0x80)) {
                    t += *i++;
                    if (i != s.end() && ((*i & 0xc0) == 0x80)) t += *i++;
                }
            } else if ((*i & 0xf8) == 0xf0) {
                t += *i++;
                if (i != s.end() && ((*i & 0xc0) == 0x80)) {
                    t += *i++;
                    if (i != s.end() && ((*i & 0xc0) == 0x80)) {
                        t += *i++;
                        if (i != s.end() && ((*i & 0xc0) == 0x80)) t += *i++;
                    }
                }
            } else
                t += *i++;
        }
    }

    return t;
}


constexpr char base64Chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

string base64Encode(std::string_view s)
{
    string res;
    int data = 0, nbits = 0;

    for (char c : s) {
        data = data << 8 | (unsigned char) c;
        nbits += 8;
        while (nbits >= 6) {
            nbits -= 6;
            res.push_back(base64Chars[data >> nbits & 0x3f]);
        }
    }

    if (nbits) res.push_back(base64Chars[data << (6 - nbits) & 0x3f]);
    while (res.size() % 4) res.push_back('=');

    return res;
}


string base64Decode(std::string_view s)
{
    constexpr char npos = -1;
    constexpr std::array<char, 256> base64DecodeChars = [&]() {
        std::array<char, 256>  result{};
        for (auto& c : result)
            c = npos;
        for (int i = 0; i < 64; i++)
            result[base64Chars[i]] = i;
        return result;
    }();

    string res;
    unsigned int d = 0, bits = 0;

    for (char c : s) {
        if (c == '=') break;
        if (c == '\n') continue;

        char digit = base64DecodeChars[(unsigned char) c];
        if (digit == npos)
            throw Error("invalid character in Base64 string: '%c'", c);

        bits += 6;
        d = d << 6 | digit;
        if (bits >= 8) {
            res.push_back(d >> (bits - 8) & 0xff);
            bits -= 8;
        }
    }

    return res;
}


std::string stripIndentation(std::string_view s)
{
    size_t minIndent = 10000;
    size_t curIndent = 0;
    bool atStartOfLine = true;

    for (auto & c : s) {
        if (atStartOfLine && c == ' ')
            curIndent++;
        else if (c == '\n') {
            if (atStartOfLine)
                minIndent = std::max(minIndent, curIndent);
            curIndent = 0;
            atStartOfLine = true;
        } else {
            if (atStartOfLine) {
                minIndent = std::min(minIndent, curIndent);
                atStartOfLine = false;
            }
        }
    }

    std::string res;

    size_t pos = 0;
    while (pos < s.size()) {
        auto eol = s.find('\n', pos);
        if (eol == s.npos) eol = s.size();
        if (eol - pos > minIndent)
            res.append(s.substr(pos + minIndent, eol - pos - minIndent));
        res.push_back('\n');
        pos = eol + 1;
    }

    return res;
}


//////////////////////////////////////////////////////////////////////


static Sync<std::pair<unsigned short, unsigned short>> windowSize{{0, 0}};


static void updateWindowSize()
{
    struct winsize ws;
    if (ioctl(2, TIOCGWINSZ, &ws) == 0) {
        auto windowSize_(windowSize.lock());
        windowSize_->first = ws.ws_row;
        windowSize_->second = ws.ws_col;
    }
}


std::pair<unsigned short, unsigned short> getWindowSize()
{
    return *windowSize.lock();
}


static Sync<std::list<std::function<void()>>> _interruptCallbacks;

static void signalHandlerThread(sigset_t set)
{
    while (true) {
        int signal = 0;
        sigwait(&set, &signal);

        if (signal == SIGINT || signal == SIGTERM || signal == SIGHUP)
            triggerInterrupt();

        else if (signal == SIGWINCH) {
            updateWindowSize();
        }
    }
}

void triggerInterrupt()
{
    _isInterrupted = true;

    {
        auto interruptCallbacks(_interruptCallbacks.lock());
        for (auto & callback : *interruptCallbacks) {
            try {
                callback();
            } catch (...) {
                ignoreException();
            }
        }
    }
}

static sigset_t savedSignalMask;

void startSignalHandlerThread()
{
    updateWindowSize();

    if (sigprocmask(SIG_BLOCK, nullptr, &savedSignalMask))
        throw SysError("querying signal mask");

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGPIPE);
    sigaddset(&set, SIGWINCH);
    if (pthread_sigmask(SIG_BLOCK, &set, nullptr))
        throw SysError("blocking signals");

    std::thread(signalHandlerThread, set).detach();
}

static void restoreSignals()
{
    if (sigprocmask(SIG_SETMASK, &savedSignalMask, nullptr))
        throw SysError("restoring signals");
}

#if __linux__
rlim_t savedStackSize = 0;
#endif

void setStackSize(size_t stackSize)
{
    #if __linux__
    struct rlimit limit;
    if (getrlimit(RLIMIT_STACK, &limit) == 0 && limit.rlim_cur < stackSize) {
        savedStackSize = limit.rlim_cur;
        limit.rlim_cur = stackSize;
        setrlimit(RLIMIT_STACK, &limit);
    }
    #endif
}

static AutoCloseFD fdSavedMountNamespace;

void saveMountNamespace()
{
#if __linux__
    static std::once_flag done;
    std::call_once(done, []() {
        AutoCloseFD fd = open("/proc/self/ns/mnt", O_RDONLY);
        if (!fd)
            throw SysError("saving parent mount namespace");
        fdSavedMountNamespace = std::move(fd);
    });
#endif
}

void restoreMountNamespace()
{
#if __linux__
    try {
        if (fdSavedMountNamespace && setns(fdSavedMountNamespace.get(), CLONE_NEWNS) == -1)
            throw SysError("restoring parent mount namespace");
    } catch (Error & e) {
        debug(e.msg());
    }
#endif
}

void restoreProcessContext(bool restoreMounts)
{
    restoreSignals();
    if (restoreMounts) {
        restoreMountNamespace();
    }

    restoreAffinity();

    #if __linux__
    if (savedStackSize) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_STACK, &limit) == 0) {
            limit.rlim_cur = savedStackSize;
            setrlimit(RLIMIT_STACK, &limit);
        }
    }
    #endif
}

/* RAII helper to automatically deregister a callback. */
struct InterruptCallbackImpl : InterruptCallback
{
    std::list<std::function<void()>>::iterator it;
    ~InterruptCallbackImpl() override
    {
        _interruptCallbacks.lock()->erase(it);
    }
};

std::unique_ptr<InterruptCallback> createInterruptCallback(std::function<void()> callback)
{
    auto interruptCallbacks(_interruptCallbacks.lock());
    interruptCallbacks->push_back(callback);

    auto res = std::make_unique<InterruptCallbackImpl>();
    res->it = interruptCallbacks->end();
    res->it--;

    return std::unique_ptr<InterruptCallback>(res.release());
}


AutoCloseFD createUnixDomainSocket()
{
    AutoCloseFD fdSocket = socket(PF_UNIX, SOCK_STREAM
        #ifdef SOCK_CLOEXEC
        | SOCK_CLOEXEC
        #endif
        , 0);
    if (!fdSocket)
        throw SysError("cannot create Unix domain socket");
    closeOnExec(fdSocket.get());
    return fdSocket;
}


AutoCloseFD createUnixDomainSocket(const Path & path, mode_t mode)
{
    auto fdSocket = nix::createUnixDomainSocket();

    bind(fdSocket.get(), path);

    if (chmod(path.c_str(), mode) == -1)
        throw SysError("changing permissions on '%1%'", path);

    if (listen(fdSocket.get(), 5) == -1)
        throw SysError("cannot listen on socket '%1%'", path);

    return fdSocket;
}


void bind(int fd, const std::string & path)
{
    unlink(path.c_str());

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;

    if (path.size() + 1 >= sizeof(addr.sun_path)) {
        Pid pid = startProcess([&]() {
            auto dir = dirOf(path);
            if (chdir(dir.c_str()) == -1)
                throw SysError("chdir to '%s' failed", dir);
            std::string base(baseNameOf(path));
            if (base.size() + 1 >= sizeof(addr.sun_path))
                throw Error("socket path '%s' is too long", base);
            memcpy(addr.sun_path, base.c_str(), base.size() + 1);
            if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1)
                throw SysError("cannot bind to socket '%s'", path);
            _exit(0);
        });
        int status = pid.wait();
        if (status != 0)
            throw Error("cannot bind to socket '%s'", path);
    } else {
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1)
            throw SysError("cannot bind to socket '%s'", path);
    }
}


void connect(int fd, const std::string & path)
{
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;

    if (path.size() + 1 >= sizeof(addr.sun_path)) {
        Pid pid = startProcess([&]() {
            auto dir = dirOf(path);
            if (chdir(dir.c_str()) == -1)
                throw SysError("chdir to '%s' failed", dir);
            std::string base(baseNameOf(path));
            if (base.size() + 1 >= sizeof(addr.sun_path))
                throw Error("socket path '%s' is too long", base);
            memcpy(addr.sun_path, base.c_str(), base.size() + 1);
            if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1)
                throw SysError("cannot connect to socket at '%s'", path);
            _exit(0);
        });
        int status = pid.wait();
        if (status != 0)
            throw Error("cannot connect to socket at '%s'", path);
    } else {
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == -1)
            throw SysError("cannot connect to socket at '%s'", path);
    }
}


string showBytes(uint64_t bytes)
{
    return fmt("%.2f MiB", bytes / (1024.0 * 1024.0));
}


// FIXME: move to libstore/build
void commonChildInit(Pipe & logPipe)
{
    logger = makeSimpleLogger();

    const static string pathNullDevice = "/dev/null";
    restoreProcessContext(false);

    /* Put the child in a separate session (and thus a separate
       process group) so that it has no controlling terminal (meaning
       that e.g. ssh cannot open /dev/tty) and it doesn't receive
       terminal signals. */
    if (setsid() == -1)
        throw SysError("creating a new session");

    /* Dup the write side of the logger pipe into stderr. */
    if (dup2(logPipe.writeSide.get(), STDERR_FILENO) == -1)
        throw SysError("cannot pipe standard error into log file");

    /* Dup stderr to stdout. */
    if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1)
        throw SysError("cannot dup stderr into stdout");

    /* Reroute stdin to /dev/null. */
    int fdDevNull = open(pathNullDevice.c_str(), O_RDWR);
    if (fdDevNull == -1)
        throw SysError("cannot open '%1%'", pathNullDevice);
    if (dup2(fdDevNull, STDIN_FILENO) == -1)
        throw SysError("cannot dup null device into stdin");
    close(fdDevNull);
}

}
