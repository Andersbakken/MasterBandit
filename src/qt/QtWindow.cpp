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
    mMaxCharWidth = w->fontMetrics().maxWidth();
    // qDebug() << "mLineSpacing" << mLineSpacing;

    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            [this](int value) {
                // qDebug() << "got scroll" << value;
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
        update(lineRect(0, *reinterpret_cast<const int *>(data)));
        break;
    }
}

void QtWindow::render(size_t x, size_t y, const char *ch, size_t len, unsigned int flags)
{
    // qDebug() << "rendering line" << y << lineRect(y) << QString::fromStdString(line);
    mPainter.drawText(lineRect(x, y), QLatin1String(ch, len));
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
    Terminal::resize(viewport()->width() / mMaxCharWidth,
                     viewport()->height() / mLineSpacing);
    updateScrollbars();
}

QRect QtWindow::lineRect(size_t x, size_t y) const
{
    return QRect(x * mMaxCharWidth, y * mLineSpacing, viewport()->width(), mLineSpacing);
}

void QtWindow::keyPressEvent(QKeyEvent *e)
{
    Terminal::keyPressEvent(createKeyEvent(e));
}

void QtWindow::keyReleaseEvent(QKeyEvent *e)
{
    Terminal::keyReleaseEvent(createKeyEvent(e));
}

void QtWindow::mousePressEvent(QMouseEvent *e)
{
    Terminal::mousePressEvent(createMouseEvent(e));
}

void QtWindow::mouseMoveEvent(QMouseEvent *e)
{
    Terminal::mouseMoveEvent(createMouseEvent(e));
}

void QtWindow::mouseReleaseEvent(QMouseEvent *e)
{
    Terminal::mouseReleaseEvent(createMouseEvent(e));
}

static unsigned int translateModifiers(const QFlags<Qt::KeyboardModifier> modifiers)
{
    unsigned int ret = 0;
    if (modifiers & Qt::ControlModifier)
        ret |= InputEvent::ControlModifier;
    if (modifiers & Qt::ShiftModifier)
        ret |= InputEvent::ShiftModifier;
    if (modifiers & Qt::AltModifier)
        ret |= InputEvent::AltModifier;
    if (modifiers & Qt::MetaModifier)
        ret |= InputEvent::MetaModifier;
    return ret;
}

KeyEvent QtWindow::createKeyEvent(QKeyEvent *e) const
{
    KeyEvent ret;
    ret.type = (e->type() == QEvent::KeyPress ? InputEvent::KeyPress : InputEvent::KeyRelease);
    ret.text = e->text().toStdString();
    ret.modifiers = translateModifiers(e->modifiers());
    // return KeyEvent { e->type() == QEvent::KeyPress ? Key::
    return ret;
}

MouseEvent QtWindow::createMouseEvent(QMouseEvent *e) const
{
}
