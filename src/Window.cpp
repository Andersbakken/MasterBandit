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

void Window::render(size_t x, size_t y, const char16_t *ch, size_t len, size_t cursor, unsigned int flags)
{
    QString string = QString::fromRawData(reinterpret_cast<const QChar *>(ch), len);
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

QRectF Window::lineRect(size_t x, size_t y, size_t chars) const
{
    return QRectF(x * mCharWidth, (y - Terminal::y()) * mLineSpacing,
                  chars == std::u16string::npos ? viewport()->width() : chars * mCharWidth,
                  mLineSpacing);
}

void Window::keyPressEvent(QKeyEvent *e)
{
    qDebug() << "press" << e->text() << e;
    Terminal::keyPressEvent(createKeyEvent(e));
}

void Window::keyReleaseEvent(QKeyEvent *e)
{
    Terminal::keyReleaseEvent(createKeyEvent(e));
}

void Window::mousePressEvent(QMouseEvent *e)
{
    Terminal::mousePressEvent(createMouseEvent(e));
}

void Window::mouseMoveEvent(QMouseEvent *e)
{
    Terminal::mouseMoveEvent(createMouseEvent(e));
}

void Window::mouseReleaseEvent(QMouseEvent *e)
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

KeyEvent Window::createKeyEvent(QKeyEvent *e) const
{
    KeyEvent ret;
    ret.type = (e->type() == QEvent::KeyPress ? InputEvent::KeyPress : InputEvent::KeyRelease);
    ret.text = e->text().toStdString();
    ret.modifiers = translateModifiers(e->modifiers());
    switch (e->key()) {
    case Qt::Key_Escape: ret.key = KeyEvent::Key_Escape; break;
    case Qt::Key_Tab: ret.key = KeyEvent::Key_Tab; break;
    case Qt::Key_Backtab: ret.key = KeyEvent::Key_Backtab; break;
    case Qt::Key_Backspace: ret.key = KeyEvent::Key_Backspace; break;
    case Qt::Key_Return: ret.key = KeyEvent::Key_Return; break;
    case Qt::Key_Enter: ret.key = KeyEvent::Key_Enter; break;
    case Qt::Key_Insert: ret.key = KeyEvent::Key_Insert; break;
    case Qt::Key_Delete: ret.key = KeyEvent::Key_Delete; break;
    case Qt::Key_Pause: ret.key = KeyEvent::Key_Pause; break;
    case Qt::Key_Print: ret.key = KeyEvent::Key_Print; break;
    case Qt::Key_SysReq: ret.key = KeyEvent::Key_SysReq; break;
    case Qt::Key_Clear: ret.key = KeyEvent::Key_Clear; break;
    case Qt::Key_Home: ret.key = KeyEvent::Key_Home; break;
    case Qt::Key_End: ret.key = KeyEvent::Key_End; break;
    case Qt::Key_Left: ret.key = KeyEvent::Key_Left; break;
    case Qt::Key_Up: ret.key = KeyEvent::Key_Up; break;
    case Qt::Key_Right: ret.key = KeyEvent::Key_Right; break;
    case Qt::Key_Down: ret.key = KeyEvent::Key_Down; break;
    case Qt::Key_PageUp: ret.key = KeyEvent::Key_PageUp; break;
    case Qt::Key_PageDown: ret.key = KeyEvent::Key_PageDown; break;
    case Qt::Key_Shift: ret.key = KeyEvent::Key_Shift; break;
    case Qt::Key_Control: ret.key = KeyEvent::Key_Control; break;
    case Qt::Key_Meta: ret.key = KeyEvent::Key_Meta; break;
    case Qt::Key_Alt: ret.key = KeyEvent::Key_Alt; break;
    case Qt::Key_CapsLock: ret.key = KeyEvent::Key_CapsLock; break;
    case Qt::Key_NumLock: ret.key = KeyEvent::Key_NumLock; break;
    case Qt::Key_ScrollLock: ret.key = KeyEvent::Key_ScrollLock; break;
    case Qt::Key_F1: ret.key = KeyEvent::Key_F1; break;
    case Qt::Key_F2: ret.key = KeyEvent::Key_F2; break;
    case Qt::Key_F3: ret.key = KeyEvent::Key_F3; break;
    case Qt::Key_F4: ret.key = KeyEvent::Key_F4; break;
    case Qt::Key_F5: ret.key = KeyEvent::Key_F5; break;
    case Qt::Key_F6: ret.key = KeyEvent::Key_F6; break;
    case Qt::Key_F7: ret.key = KeyEvent::Key_F7; break;
    case Qt::Key_F8: ret.key = KeyEvent::Key_F8; break;
    case Qt::Key_F9: ret.key = KeyEvent::Key_F9; break;
    case Qt::Key_F10: ret.key = KeyEvent::Key_F10; break;
    case Qt::Key_F11: ret.key = KeyEvent::Key_F11; break;
    case Qt::Key_F12: ret.key = KeyEvent::Key_F12; break;
    case Qt::Key_F13: ret.key = KeyEvent::Key_F13; break;
    case Qt::Key_F14: ret.key = KeyEvent::Key_F14; break;
    case Qt::Key_F15: ret.key = KeyEvent::Key_F15; break;
    case Qt::Key_F16: ret.key = KeyEvent::Key_F16; break;
    case Qt::Key_F17: ret.key = KeyEvent::Key_F17; break;
    case Qt::Key_F18: ret.key = KeyEvent::Key_F18; break;
    case Qt::Key_F19: ret.key = KeyEvent::Key_F19; break;
    case Qt::Key_F20: ret.key = KeyEvent::Key_F20; break;
    case Qt::Key_F21: ret.key = KeyEvent::Key_F21; break;
    case Qt::Key_F22: ret.key = KeyEvent::Key_F22; break;
    case Qt::Key_F23: ret.key = KeyEvent::Key_F23; break;
    case Qt::Key_F24: ret.key = KeyEvent::Key_F24; break;
    case Qt::Key_F25: ret.key = KeyEvent::Key_F25; break;
    case Qt::Key_F26: ret.key = KeyEvent::Key_F26; break;
    case Qt::Key_F27: ret.key = KeyEvent::Key_F27; break;
    case Qt::Key_F28: ret.key = KeyEvent::Key_F28; break;
    case Qt::Key_F29: ret.key = KeyEvent::Key_F29; break;
    case Qt::Key_F30: ret.key = KeyEvent::Key_F30; break;
    case Qt::Key_F31: ret.key = KeyEvent::Key_F31; break;
    case Qt::Key_F32: ret.key = KeyEvent::Key_F32; break;
    case Qt::Key_F33: ret.key = KeyEvent::Key_F33; break;
    case Qt::Key_F34: ret.key = KeyEvent::Key_F34; break;
    case Qt::Key_F35: ret.key = KeyEvent::Key_F35; break;
    case Qt::Key_Super_L: ret.key = KeyEvent::Key_Super_L; break;
    case Qt::Key_Super_R: ret.key = KeyEvent::Key_Super_R; break;
    case Qt::Key_Menu: ret.key = KeyEvent::Key_Menu; break;
    case Qt::Key_Hyper_L: ret.key = KeyEvent::Key_Hyper_L; break;
    case Qt::Key_Hyper_R: ret.key = KeyEvent::Key_Hyper_R; break;
    case Qt::Key_Help: ret.key = KeyEvent::Key_Help; break;
        // case Qt::Key_Direction: ret.key = KeyEvent::Key_Direction; break;
        // case Qt::Key_Direction: ret.key = KeyEvent::Key_Direction; break;
    case Qt::Key_Space: ret.key = KeyEvent::Key_Space; break;
    // case Qt::Key_Any: ret.key = KeyEvent::Key_Any; break;
    case Qt::Key_Exclam: ret.key = KeyEvent::Key_Exclam; break;
    case Qt::Key_QuoteDbl: ret.key = KeyEvent::Key_QuoteDbl; break;
    case Qt::Key_NumberSign: ret.key = KeyEvent::Key_Hash; break; // ### different
    case Qt::Key_Dollar: ret.key = KeyEvent::Key_Dollar; break;
    case Qt::Key_Percent: ret.key = KeyEvent::Key_Percent; break;
    case Qt::Key_Ampersand: ret.key = KeyEvent::Key_Ampersand; break;
    case Qt::Key_Apostrophe: ret.key = KeyEvent::Key_Apostrophe; break;
    case Qt::Key_ParenLeft: ret.key = KeyEvent::Key_OpenParen; break;
    case Qt::Key_ParenRight: ret.key = KeyEvent::Key_CloseParen; break;
    case Qt::Key_Asterisk: ret.key = KeyEvent::Key_Asterisk; break;
    case Qt::Key_Plus: ret.key = KeyEvent::Key_Plus; break;
    case Qt::Key_Comma: ret.key = KeyEvent::Key_Comma; break;
    case Qt::Key_Minus: ret.key = KeyEvent::Key_Minus; break;
    case Qt::Key_Period: ret.key = KeyEvent::Key_Period; break;
    case Qt::Key_Slash: ret.key = KeyEvent::Key_Slash; break;
    case Qt::Key_0: ret.key = KeyEvent::Key_0; break;
    case Qt::Key_1: ret.key = KeyEvent::Key_1; break;
    case Qt::Key_2: ret.key = KeyEvent::Key_2; break;
    case Qt::Key_3: ret.key = KeyEvent::Key_3; break;
    case Qt::Key_4: ret.key = KeyEvent::Key_4; break;
    case Qt::Key_5: ret.key = KeyEvent::Key_5; break;
    case Qt::Key_6: ret.key = KeyEvent::Key_6; break;
    case Qt::Key_7: ret.key = KeyEvent::Key_7; break;
    case Qt::Key_8: ret.key = KeyEvent::Key_8; break;
    case Qt::Key_9: ret.key = KeyEvent::Key_9; break;
    case Qt::Key_Colon: ret.key = KeyEvent::Key_Colon; break;
    case Qt::Key_Semicolon: ret.key = KeyEvent::Key_Semicolon; break;
    case Qt::Key_Less: ret.key = KeyEvent::Key_Less; break;
    case Qt::Key_Equal: ret.key = KeyEvent::Key_Equal; break;
    case Qt::Key_Greater: ret.key = KeyEvent::Key_Greater; break;
    case Qt::Key_Question: ret.key = KeyEvent::Key_Question; break;
    case Qt::Key_At: ret.key = KeyEvent::Key_At; break;
    case Qt::Key_A: ret.key = KeyEvent::Key_A; break;
    case Qt::Key_B: ret.key = KeyEvent::Key_B; break;
    case Qt::Key_C: ret.key = KeyEvent::Key_C; break;
    case Qt::Key_D: ret.key = KeyEvent::Key_D; break;
    case Qt::Key_E: ret.key = KeyEvent::Key_E; break;
    case Qt::Key_F: ret.key = KeyEvent::Key_F; break;
    case Qt::Key_G: ret.key = KeyEvent::Key_G; break;
    case Qt::Key_H: ret.key = KeyEvent::Key_H; break;
    case Qt::Key_I: ret.key = KeyEvent::Key_I; break;
    case Qt::Key_J: ret.key = KeyEvent::Key_J; break;
    case Qt::Key_K: ret.key = KeyEvent::Key_K; break;
    case Qt::Key_L: ret.key = KeyEvent::Key_L; break;
    case Qt::Key_M: ret.key = KeyEvent::Key_M; break;
    case Qt::Key_N: ret.key = KeyEvent::Key_N; break;
    case Qt::Key_O: ret.key = KeyEvent::Key_O; break;
    case Qt::Key_P: ret.key = KeyEvent::Key_P; break;
    case Qt::Key_Q: ret.key = KeyEvent::Key_Q; break;
    case Qt::Key_R: ret.key = KeyEvent::Key_R; break;
    case Qt::Key_S: ret.key = KeyEvent::Key_S; break;
    case Qt::Key_T: ret.key = KeyEvent::Key_T; break;
    case Qt::Key_U: ret.key = KeyEvent::Key_U; break;
    case Qt::Key_V: ret.key = KeyEvent::Key_V; break;
    case Qt::Key_W: ret.key = KeyEvent::Key_W; break;
    case Qt::Key_X: ret.key = KeyEvent::Key_X; break;
    case Qt::Key_Y: ret.key = KeyEvent::Key_Y; break;
    case Qt::Key_Z: ret.key = KeyEvent::Key_Z; break;
    case Qt::Key_BracketLeft: ret.key = KeyEvent::Key_OpenBracket; break;
    case Qt::Key_BracketRight: ret.key = KeyEvent::Key_CloseBracket; break;
    case Qt::Key_Backslash: ret.key = KeyEvent::Key_Backslash; break;
        // case Qt::Key_AsciiCircum: ret.key = KeyEvent::Key_AsciiCircum; break;
    case Qt::Key_Underscore: ret.key = KeyEvent::Key_Underscore; break;
    case Qt::Key_QuoteLeft: ret.key = KeyEvent::Key_BackTick; break; // ### is this right?
    case Qt::Key_BraceLeft: ret.key = KeyEvent::Key_OpenBrace; break;
        // case Qt::Key_Bar: ret.key = KeyEvent::Key_Bar; break;
    case Qt::Key_BraceRight: ret.key = KeyEvent::Key_CloseBrace; break;
    case Qt::Key_AsciiTilde: ret.key = KeyEvent::Key_Tilde; break;
    default:
        DEBUG("Unhandled Qt key %d", e->key());
        break;
    };
    ret.autoRepeat = e->isAutoRepeat();
    ret.count = e->count();
    return ret;
}

MouseEvent Window::createMouseEvent(QMouseEvent *e) const
{
    MouseEvent ret;
    switch (e->type()) {
    case QEvent::MouseButtonPress:
        ret.type = InputEvent::MousePress;
        break;
    case QEvent::MouseButtonRelease:
        ret.type = InputEvent::MouseRelease;
        break;
    case QEvent::MouseMove:
        ret.type = InputEvent::MouseMove;
        break;
    default:
        abort();
        break;
    }
    // ret.type = (e->type() == QEvent::MoPress ? InputEvent::KeyPress : InputEvent::KeyRelease);
    // // ret.text = e->text().toStdString();
    // ret.modifiers = translateModifiers(e->modifiers());
    return ret;
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
