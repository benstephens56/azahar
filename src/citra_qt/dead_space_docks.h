// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <vector>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QStringList>
#include <QTimer>

class GRenderWindow;
class QDockWidget;
class QMainWindow;
class QRubberBand;

/**
 * Lets tool windows dock into the empty (black) parts of the render window that the screen layout
 * leaves, like next to the bottom screen. Drag a floating tool window over an empty part: it's
 * highlighted, and letting go there snaps the window into it. Docked windows follow the main
 * window and layout changes, share an empty part when there are several in it, and are undocked
 * by dragging them away.
 */
class DeadSpaceDocks : public QObject {
    Q_OBJECT

public:
    DeadSpaceDocks(QMainWindow* main_window, GRenderWindow* render_window);
    ~DeadSpaceDocks() override;

    void AddDock(QDockWidget* dock);

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    /// The empty parts of the render window, in global coordinates, largest first
    std::vector<QRect> FindRegions() const;
    /// Index of the region containing the point, or -1
    int RegionAt(const QPoint& point) const;
    void Refresh();
    void PlaceDocks();
    void Attach(QDockWidget* dock, int region);
    void Detach(QDockWidget* dock);
    void OnDragSettled();
    void Save() const;

    QMainWindow* main_window;
    GRenderWindow* render_window;
    std::vector<QDockWidget*> docks;
    /// Docked windows and the region they're in
    std::map<QDockWidget*, int> attached;
    /// Docks to attach (by object name and region) once the render window shows empty parts
    std::map<QString, int> pending;
    std::vector<QRect> regions;
    QTimer poll_timer;
    QTimer settle_timer;
    QPointer<QDockWidget> dragging;
    QRubberBand* highlight;
    bool placing = false;
    bool hidden = false;
};
