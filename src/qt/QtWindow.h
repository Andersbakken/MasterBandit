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
    virtual void render(size_t y, const std::string &line) override;

    // QAbstractScrollArea
    using QWidget::event;
    virtual void paintEvent(QPaintEvent *e) override;
    virtual void showEvent(QShowEvent *e) override;
    virtual void resizeEvent(QResizeEvent *e) override;
private:
    void updateScrollbars();
    QRect lineRect(int y) const;
private:
    QPainter mPainter;
    int mLineSpacing { 0 };
};

#endif /* QTWINDOW_H */
