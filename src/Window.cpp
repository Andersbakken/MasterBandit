#include "Window.h"
#include "Log.h"
#include <assert.h>

Window::Window()
    : Terminal()
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

void Window::render(int x, int y, const QString &str, int start, int len, int cursor, unsigned int flags)
{
    QString string = QString::fromRawData(str.constData() + start, len);
    if (cursor != std::u16string::npos) {
        QRectF r = lineRect(cursor, y, 1);
        mPainter.fillRect(r, Qt::darkGray);
    }

    mPainter.drawText(lineRect(x, y), string);
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
    qDebug() << "got resize" << e << Terminal::width() << Terminal::height();
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
    Terminal::keyPressEvent(e);
}

void Window::keyReleaseEvent(QKeyEvent *e)
{
    Terminal::keyReleaseEvent(e);
}

void Window::mousePressEvent(QMouseEvent *e)
{
    Terminal::mousePressEvent(e);
}

void Window::mouseMoveEvent(QMouseEvent *e)
{
    Terminal::mouseMoveEvent(e);
}

void Window::mouseReleaseEvent(QMouseEvent *e)
{
    Terminal::mouseReleaseEvent(e);
}

bool Window::init(const Options &options)
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

void Window::quit()
{
    QApplication::quit();
}
