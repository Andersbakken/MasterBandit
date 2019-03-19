#include "QtWindow.h"
#include <assert.h>

QtWindow::QtWindow()
    : Terminal()
{
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);

    QWidget *w = viewport();
    QFont f = w->font();
    f.setFamily("monospace");
    f.setStyleHint(QFont::Monospace);
    w->setFont(f);
    mLineSpacing = w->fontMetrics().lineSpacing();
    // qDebug() << "mLineSpacing" << mLineSpacing;

    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            [this](int value) {
                qDebug() << "got scroll" << value;
                Terminal::scroll(0, value);
            });

}

void QtWindow::paintEvent(QPaintEvent *e)
{
    mPainter.begin(viewport());
    mPainter.fillRect(e->rect(), Qt::black);
    mPainter.setPen(QPen(Qt::white));
    Terminal::render();
    mPainter.end();
}

void QtWindow::event(Event event, void *data)
{
    switch (event) {
    case ScrollbackChanged:
        updateScrollbars();
        break;
    case Update:
        update();
        break;
    case LineChanged:
        assert(data);
        update(lineRect(*reinterpret_cast<const int *>(data)));
        break;
    }
}

void QtWindow::render(size_t y, const std::string &line)
{
    // qDebug() << "rendering line" << y << lineRect(y) << QString::fromStdString(line);
    mPainter.drawText(lineRect(y), QString::fromStdString(line));
}

void QtWindow::showEvent(QShowEvent *e)
{
    QAbstractScrollArea::showEvent(e);
    updateScrollbars();
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void QtWindow::updateScrollbars()
{
    // const QFontMetrics metrics = viewport()->fontMetrics();
    // int lineSpacing = metrics.lineSpacing();
    if (Terminal::height() >= Terminal::scrollBackLength()) {
        verticalScrollBar()->setRange(0, 0);
    } else {
        verticalScrollBar()->setRange(0, Terminal::scrollBackLength() - Terminal::height());
    }
}

void QtWindow::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    const QFontMetrics metrics = viewport()->fontMetrics();
    const int lineSpacing = metrics.lineSpacing();
    const int charWidth = metrics.maxWidth();
    Terminal::resize(viewport()->width() / charWidth,
                     viewport()->height() / lineSpacing);
    updateScrollbars();
}

QRect QtWindow::lineRect(int y) const
{
    return QRect(0, y * mLineSpacing, viewport()->width(), mLineSpacing);
}

