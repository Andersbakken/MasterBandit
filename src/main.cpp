#include <unistd.h>
#include "Terminal.h"
#include "Window.h"
#include <getopt.h>
#include <regex>
#include "Log.h"

void usage(FILE *f)
{
    fprintf(f, "Usage: mb [-v] [-s shell]\n");
    fflush(f);
}

static std::string defaultShell(const std::string &user)
{
    if (const char *shell = getenv("SHELL")) {
        return shell;
    }
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        std::string r;
        char line[1024];
        std::regex rx("^" + user + ":[^:]*:[^:]*:[^:]*:[^:]*:[^:]*:([^: ]+) *$");
        while ((fgets(line, sizeof(line), f))) {
            std::smatch m;
            std::string l(line, strlen(line) - 1);
            if (std::regex_search(l, m, rx)) {
                r = std::move(m[1]);
                break;
            }
        }
        fclose(f);
        if (!r.empty())
            return r;
    } else {
        fprintf(stderr, "Failed to open /etc/passwd %d %s\n", errno, strerror(errno));
    }
    return "/bin/dash";
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Terminal::Options options;
    char buf[1024];
    if (!getlogin_r(buf, sizeof(buf))) {
        options.user = buf;
    } else if (const char *u = getenv("USER")) {
        options.user = u;
    } else if (const char *un = getenv("USERNAME")) {
        options.user = un;
    } else {
        fprintf(stderr, "Can't find user\n");
        return 1;
    }

    options.shell = defaultShell(options.user);
    int logLevel = Log::logLevel();
    int opt;
    while ((opt = getopt(argc, argv, ":vs:")) != -1) {
        switch (opt) {
        case 'v':
            if (logLevel > 0)
                --logLevel;
            break;
        case 's':
            options.shell = optarg;
            break;
        default:
            usage(stderr);
            return 2;
        }
    }
    Log::setLogLevel(static_cast<Log::Level>(logLevel));

    Window window;
    if (!window.init(options))
        return 3;
    // QFile file("../LICENSE");
    // file.open(QIODevice::ReadOnly);
    // const std::string str = file.readAll().toStdString();
    // file.close();
    // window.addText(str);
    window.show();
    return app.exec();
}
