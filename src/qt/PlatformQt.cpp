#include <Terminal.h>
#include <Platform.h>
#include <Log.h>
#include <QtWidgets>

class Window : public QAbstractScrollArea, public Terminal
{
    Q_OBJECT
public:
    Window(Platform *platform);

    virtual bool init(const TerminalOptions &options) override;

protected:
    // Terminal
    virtual void event(Event event, void *info) override;
    virtual void render(int x, int y, const std::string &string, int start, int len, int cursor, unsigned int flags) override;

    // QAbstractScrollArea
    using QWidget::event;
    virtual void paintEvent(QPaintEvent *e) override;
    virtual void showEvent(QShowEvent *e) override;
    virtual void resizeEvent(QResizeEvent *e) override;
    virtual void keyPressEvent(QKeyEvent *e) override;
    virtual void keyReleaseEvent(QKeyEvent *e) override;
    virtual void mousePressEvent(QMouseEvent *e) override;
    virtual void mouseMoveEvent(QMouseEvent *e) override;
    virtual void mouseReleaseEvent(QMouseEvent *e) override;
private:
    void updateScrollbars();
    QRectF lineRect(int x, int y, int chars = -1) const;
private:
    QPainter mPainter;
    double mLineSpacing { 0 };
    double mCharWidth { 0 };
    bool mBell { false };
};

class PlatformQt : public Platform, public QApplication
{
public:
    PlatformQt(int argc, char **argv)
        : QApplication(argc, argv)
    {}
    virtual int exec() override
    {
        return QApplication::exec();
    }

    virtual void quit(int code) override
    {
        QApplication::exit(code);
    }

    virtual std::unique_ptr<Terminal> createTerminal(const TerminalOptions &options) override
    {
        std::unique_ptr<Window> window(new Window(this));
        if (!window->init(options))
            return nullptr;
        window->show();
        return window;
    }
};

std::unique_ptr<Platform> createPlatform(int argc, char **argv)
{
    return std::unique_ptr<Platform>(new PlatformQt(argc, argv));
}

static KeyEvent createKeyEvent(const QKeyEvent *ev)
{
    KeyEvent ret;
    ret.key = static_cast<Key>(ev->key());
    ret.count = ev->count();
    ret.text = ev->text().toStdString();
    return ret;
}

static MouseEvent createMouseEvent(const QMouseEvent *ev)
{
    MouseEvent ret;
    ret.x = ev->x();
    ret.y = ev->y();
    return ret;
}

Window::Window(Platform *platform)
    : Terminal(platform)
{
    QWidget *w = viewport();
    QFont f = w->font();
    f.setFamily("monospace");
    f.setStyleHint(QFont::Monospace);
    w->setFont(f);
    QFontMetricsF fm(f);
    mLineSpacing = fm.lineSpacing();
    mCharWidth = fm.width('M');

    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            [this](int value) {
                // qDebug() << "got scroll" << value;
                Terminal::scroll(0, value);
            });

}

void Window::paintEvent(QPaintEvent *e)
{
    mPainter.begin(viewport());
    if (mBell) {
        mPainter.fillRect(viewport()->rect(), Qt::white);
        mBell = false;
        viewport()->update();
    } else {
        mPainter.fillRect(e->rect(), Qt::black);
        mPainter.setPen(QPen(Qt::white));
        Terminal::render();
    }
    mPainter.end();
}

void Window::event(Event event, void *data)
{
    VERBOSE("Got Window::event(%s)", eventName(event));
    switch (event) {
    case ScrollbackChanged:
        updateScrollbars();
        break;
    case Update:
        viewport()->update();
        break;
    case VisibleBell:
        mBell = true;
        update();
        break;
    }
}

void Window::render(int x, int y, const std::string &str, int start, int len, int cursor, unsigned int flags)
{
    std::string string = str.substr(start, len);
    if (cursor != std::u16string::npos) {
        QRectF r = lineRect(cursor, y, 1);
        mPainter.fillRect(r, Qt::darkGray);
    }

    mPainter.drawText(lineRect(x, y), QString::fromStdString(string));
}

void Window::showEvent(QShowEvent *e)
{
    QAbstractScrollArea::showEvent(e);
    updateScrollbars();
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void Window::updateScrollbars()
{
    // const QFontMetrics metrics = viewport()->fontMetrics();
    // int lineSpacing = metrics.lineSpacing();
    if (Terminal::height() >= Terminal::scrollBackLength()) {
        verticalScrollBar()->setRange(0, 0);
    } else {
        const bool atEnd = verticalScrollBar()->value() == verticalScrollBar()->maximum();
        verticalScrollBar()->setRange(0, Terminal::scrollBackLength() - Terminal::height());
        if (atEnd)
            verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    }
}

void Window::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    Terminal::resize(viewport()->width() / mCharWidth,
                     viewport()->height() / mLineSpacing);
    // qDebug() << "got resize" << e << Terminal::width() << Terminal::height();
    updateScrollbars();
}

QRectF Window::lineRect(int x, int y, int chars) const
{
    return QRectF(x * mCharWidth, (y - Terminal::y()) * mLineSpacing,
                  chars == std::u16string::npos ? viewport()->width() : chars * mCharWidth,
                  mLineSpacing);
}

void Window::keyPressEvent(QKeyEvent *e)
{
    KeyEvent ke = createKeyEvent(e);
    Terminal::keyPressEvent(&ke);
}

void Window::keyReleaseEvent(QKeyEvent *e)
{
    KeyEvent ke = createKeyEvent(e);
    Terminal::keyReleaseEvent(&ke);
}

void Window::mousePressEvent(QMouseEvent *e)
{
    MouseEvent me = createMouseEvent(e);
    Terminal::mousePressEvent(&me);
}

void Window::mouseMoveEvent(QMouseEvent *e)
{
    MouseEvent me = createMouseEvent(e);
    Terminal::mouseMoveEvent(&me);
}

void Window::mouseReleaseEvent(QMouseEvent *e)
{
    MouseEvent me = createMouseEvent(e);
    Terminal::mouseReleaseEvent(&me);
}

bool Window::init(const TerminalOptions &options)
{
    if (!Terminal::init(options))
        return false;

    QSocketNotifier *notifier = new QSocketNotifier(masterFD(), QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, [this](int) {
        // ERROR("GOT SOCKET NOTIFIER");
        readFromFD();
    });
    return true;
}

#include "PlatformQt.moc"
