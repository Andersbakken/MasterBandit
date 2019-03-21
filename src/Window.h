#ifndef QTWINDOW_H
#define QTWINDOW_H

#include <Terminal.h>
#include <QtWidgets>

class Window : public QAbstractScrollArea, public Terminal
{
    Q_OBJECT
public:
    Window();

    virtual bool init(const Options &options) override;

protected:
    // Terminal
    virtual void event(Event event, void *info) override;
    virtual void render(int x, int y, const QString &string, int start, int len, int cursor, unsigned int flags) override;
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
    QRectF lineRect(int x, int y, int chars = -1) const;
private:
    QPainter mPainter;
    double mLineSpacing { 0 };
    double mCharWidth { 0 };
    bool mBell { false };
};

#endif /* QTWINDOW_H */
