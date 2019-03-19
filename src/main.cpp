#include <unistd.h>
#include "Terminal.h"
#include "qt/QtWindow.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QtWindow window;
    QFile file("../LICENSE");
    file.open(QIODevice::ReadOnly);
    const std::string str = file.readAll().toStdString();
    file.close();
    window.addText(str);
    window.show();
    return app.exec();
}
