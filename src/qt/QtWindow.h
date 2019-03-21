#ifndef QTWINDOW_H
#define QTWINDOW_H

#include <Terminal.h>
#include <QtWidgets>

class QtWindow : public QAbstractScrollArea, public Terminal
{
    Q_OBJECT
public:
    QtWindow();

    virtual bool init(const Options &options) override;

protected:
    // Terminal
    virtual void event(Event event, void *info) override;
    virtual void render(size_t x, size_t y, const char16_t *ch, size_t len, size_t cursor, unsigned int flags) override;
    virtual void quit() override;

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
    QRectF lineRect(size_t x, size_t y, size_t chars = std::u16string::npos) const;
    KeyEvent createKeyEvent(QKeyEvent *e) const;
    MouseEvent createMouseEvent(QMouseEvent *e) const;
private:
    QPainter mPainter;
    double mLineSpacing { 0 };
    double mCharWidth { 0 };
};

#endif /* QTWINDOW_H */
