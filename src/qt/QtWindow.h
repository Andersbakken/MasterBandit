#ifndef QTWINDOW_H
#define QTWINDOW_H

#include <Terminal.h>
#include <QtWidgets>

class QtWindow : public QAbstractScrollArea, public Terminal
{
    Q_OBJECT
public:
    QtWindow();

protected:
    // Terminal
    virtual void event(Event event, void *info) override;
    virtual void render(size_t x, size_t y, const char *ch, size_t len, unsigned int flags) override;

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
    QRect lineRect(size_t x, size_t y) const;
    KeyEvent createKeyEvent(QKeyEvent *e) const;
    MouseEvent createMouseEvent(QMouseEvent *e) const;
private:
    QPainter mPainter;
    size_t mLineSpacing { 0 }, mMaxCharWidth { 0 };
};

#endif /* QTWINDOW_H */
