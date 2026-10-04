// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <QCursor>
#include <QDockWidget>
#include <QEvent>
#include <QGuiApplication>
#include <QMainWindow>
#include <QRubberBand>
#include <QSettings>
#include <fmt/format.h>
#include "citra_qt/bootmanager.h"
#include "citra_qt/dead_space_docks.h"
#include "common/file_util.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

/// Smallest empty part (in logical pixels) worth docking into
constexpr int MinRegionWidth = 150;
constexpr int MinRegionHeight = 100;

QString SettingsPath() {
    return QString::fromStdString(fmt::format(
        "{}dead_space_docks.ini", FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir)));
}

bool LeftButtonDown() {
#ifdef _WIN32
    // Qt doesn't see the button while a window is dragged by its native title bar
    return (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) !=
           0;
#else
    return QGuiApplication::mouseButtons() & Qt::LeftButton;
#endif
}

} // namespace

DeadSpaceDocks::DeadSpaceDocks(QMainWindow* main_window_, GRenderWindow* render_window_)
    : QObject(main_window_), main_window{main_window_}, render_window{render_window_} {
    highlight = new QRubberBand(QRubberBand::Rectangle);
    highlight->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);

    QSettings settings(SettingsPath(), QSettings::IniFormat);
    const int count = settings.beginReadArray(QStringLiteral("docks"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        pending[settings.value(QStringLiteral("name")).toString()] =
            settings.value(QStringLiteral("region")).toInt();
    }
    settings.endArray();

    main_window->installEventFilter(this);
    render_window->installEventFilter(this);
    // Layout changes (the screen layout option, swapping screens) don't come with an event
    poll_timer.setInterval(200);
    connect(&poll_timer, &QTimer::timeout, this, &DeadSpaceDocks::Refresh);
    poll_timer.start();
    settle_timer.setSingleShot(true);
    settle_timer.setInterval(300);
    connect(&settle_timer, &QTimer::timeout, this, &DeadSpaceDocks::OnDragSettled);
}

DeadSpaceDocks::~DeadSpaceDocks() {
    delete highlight;
}

void DeadSpaceDocks::AddDock(QDockWidget* dock) {
    if (std::find(docks.begin(), docks.end(), dock) != docks.end()) {
        return;
    }
    docks.push_back(dock);
    dock->installEventFilter(this);
    connect(dock, &QDockWidget::topLevelChanged, this, [this, dock](bool floating) {
        // Docked into the main window's sides
        if (!floating && attached.contains(dock)) {
            Detach(dock);
        }
    });
    connect(dock, &QObject::destroyed, this, [this, dock] {
        attached.erase(dock);
        std::erase(docks, dock);
    });
}

std::vector<QRect> DeadSpaceDocks::FindRegions() const {
    if (!render_window->isVisible() || main_window->isMinimized()) {
        return {};
    }
    const auto& layout = render_window->GetFramebufferLayout();
    if (layout.width == 0 || layout.height == 0) {
        return {};
    }
    std::vector<Common::Rectangle<u32>> screens;
    if (layout.top_screen_enabled) {
        screens.push_back(layout.top_screen);
    }
    if (layout.bottom_screen_enabled) {
        screens.push_back(layout.bottom_screen);
    }
    if (layout.additional_screen_enabled) {
        screens.push_back(layout.additional_screen);
    }

    // Split the window along every screen edge into a grid of cells, each either covered by a
    // screen or empty
    std::vector<u32> xs{0, layout.width};
    std::vector<u32> ys{0, layout.height};
    for (const auto& screen : screens) {
        xs.push_back(std::min(screen.left, layout.width));
        xs.push_back(std::min(screen.right, layout.width));
        ys.push_back(std::min(screen.top, layout.height));
        ys.push_back(std::min(screen.bottom, layout.height));
    }
    for (auto* edges : {&xs, &ys}) {
        std::sort(edges->begin(), edges->end());
        edges->erase(std::unique(edges->begin(), edges->end()), edges->end());
    }
    const int columns = static_cast<int>(xs.size()) - 1;
    const int rows = static_cast<int>(ys.size()) - 1;
    const auto empty = [&](int column, int row) {
        const u32 x = (xs[column] + xs[column + 1]) / 2;
        const u32 y = (ys[row] + ys[row + 1]) / 2;
        return std::none_of(screens.begin(), screens.end(), [&](const auto& screen) {
            return x >= screen.left && x < screen.right && y >= screen.top && y < screen.bottom;
        });
    };

    // Every rectangle of empty cells, in logical pixels, then the largest ones that don't overlap
    const qreal ratio = render_window->devicePixelRatioF();
    std::vector<QRect> candidates;
    for (int c0 = 0; c0 < columns; ++c0) {
        for (int r0 = 0; r0 < rows; ++r0) {
            for (int c1 = c0; c1 < columns; ++c1) {
                for (int r1 = r0; r1 < rows; ++r1) {
                    bool all_empty = true;
                    for (int c = c0; c <= c1 && all_empty; ++c) {
                        for (int r = r0; r <= r1 && all_empty; ++r) {
                            all_empty = empty(c, r);
                        }
                    }
                    if (!all_empty) {
                        continue;
                    }
                    const QPoint top_left(qRound(xs[c0] / ratio), qRound(ys[r0] / ratio));
                    const QPoint bottom_right(qRound(xs[c1 + 1] / ratio),
                                              qRound(ys[r1 + 1] / ratio));
                    const QRect rect(top_left, bottom_right - QPoint(1, 1));
                    if (rect.width() >= MinRegionWidth && rect.height() >= MinRegionHeight) {
                        candidates.push_back(rect);
                    }
                }
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const QRect& a, const QRect& b) {
        return a.width() * a.height() > b.width() * b.height();
    });
    std::vector<QRect> result;
    for (const QRect& candidate : candidates) {
        if (std::none_of(result.begin(), result.end(),
                         [&](const QRect& taken) { return taken.intersects(candidate); })) {
            result.push_back(candidate);
        }
    }
    for (QRect& rect : result) {
        rect.moveTopLeft(render_window->mapToGlobal(rect.topLeft()));
    }
    return result;
}

int DeadSpaceDocks::RegionAt(const QPoint& point) const {
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].contains(point)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void DeadSpaceDocks::Refresh() {
    auto found = FindRegions();
    if (found.empty()) {
        // No empty parts to show the docked windows in (like with the game list shown)
        regions.clear();
        if (!hidden && !attached.empty()) {
            hidden = true;
            for (const auto& [dock, region] : attached) {
                dock->hide();
            }
        }
        return;
    }
    const bool changed = found != regions;
    regions = std::move(found);
    if (hidden) {
        hidden = false;
        for (const auto& [dock, region] : attached) {
            dock->show();
        }
    }
    // Docks docked in an earlier session
    for (auto it = pending.begin(); it != pending.end();) {
        const auto dock = std::find_if(docks.begin(), docks.end(), [&](QDockWidget* d) {
            return d->objectName() == it->first;
        });
        if (dock != docks.end() && !(*dock)->isHidden()) {
            if (!(*dock)->isFloating()) {
                (*dock)->setFloating(true);
            }
            attached[*dock] = it->second;
            it = pending.erase(it);
        } else {
            ++it;
        }
    }
    if (changed || !attached.empty()) {
        PlaceDocks();
    }
}

void DeadSpaceDocks::PlaceDocks() {
    if (regions.empty()) {
        return;
    }
    // Docks per region (a region that no longer exists falls back to the largest)
    std::map<int, std::vector<QDockWidget*>> by_region;
    for (const auto& [dock, region] : attached) {
        if (dock->isVisible()) {
            by_region[region < static_cast<int>(regions.size()) ? region : 0].push_back(dock);
        }
    }
    placing = true;
    for (auto& [region, in_region] : by_region) {
        // Same order every time
        std::sort(in_region.begin(), in_region.end(),
                  [](QDockWidget* a, QDockWidget* b) { return a->objectName() < b->objectName(); });
        const QRect area = regions[region];
        const bool split_vertically = area.height() >= area.width();
        const int count = static_cast<int>(in_region.size());
        for (int i = 0; i < count; ++i) {
            QRect part = area;
            if (split_vertically) {
                part.setTop(area.top() + area.height() * i / count);
                part.setBottom(area.top() + area.height() * (i + 1) / count - 1);
            } else {
                part.setLeft(area.left() + area.width() * i / count);
                part.setRight(area.left() + area.width() * (i + 1) / count - 1);
            }
            // Fit the window frame (title bar and borders) inside the part too
            QDockWidget* dock = in_region[i];
            const QRect inner = dock->geometry();
            const QRect outer = dock->frameGeometry();
            const QMargins frame(inner.left() - outer.left(), inner.top() - outer.top(),
                                 outer.right() - inner.right(), outer.bottom() - inner.bottom());
            const QRect target = part.marginsRemoved(frame);
            if (dock->geometry() != target) {
                dock->setGeometry(target);
            }
        }
    }
    placing = false;
}

void DeadSpaceDocks::Attach(QDockWidget* dock, int region) {
    if (!dock->isFloating()) {
        dock->setFloating(true);
    }
    attached[dock] = region;
    dock->show();
    dock->raise();
    PlaceDocks();
    Save();
}

void DeadSpaceDocks::Detach(QDockWidget* dock) {
    if (attached.erase(dock)) {
        Save();
    }
}

bool DeadSpaceDocks::eventFilter(QObject* object, QEvent* event) {
    if (object == main_window || object == render_window) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::WindowStateChange:
            QTimer::singleShot(0, this, &DeadSpaceDocks::Refresh);
            break;
        default:
            break;
        }
        return false;
    }
    auto* dock = qobject_cast<QDockWidget*>(object);
    if (!dock) {
        return false;
    }
    // Only moves while the user drags the window count (not ones when it's restored or placed)
    const bool user_drag = LeftButtonDown() && dock->frameGeometry().contains(QCursor::pos());
    if (event->type() == QEvent::Move && dock->isFloating() && !placing && dock->isVisible() &&
        user_drag) {
        // Moved by the user: undock it, and dock it where it's let go if that's an empty part
        Detach(dock);
        dragging = dock;
        const int region = RegionAt(QCursor::pos());
        if (region >= 0) {
            highlight->setGeometry(regions[region]);
            highlight->show();
            highlight->raise();
        } else {
            highlight->hide();
        }
        settle_timer.start();
    } else if (event->type() == QEvent::Close) {
        Detach(dock);
    }
    return false;
}

void DeadSpaceDocks::OnDragSettled() {
    if (!dragging) {
        highlight->hide();
        return;
    }
    if (LeftButtonDown()) {
        // Still being dragged
        settle_timer.start();
        return;
    }
    highlight->hide();
    const int region = RegionAt(QCursor::pos());
    QDockWidget* dock = dragging;
    dragging = nullptr;
    if (region >= 0 && dock->isFloating()) {
        Attach(dock, region);
    }
}

void DeadSpaceDocks::Save() const {
    QSettings settings(SettingsPath(), QSettings::IniFormat);
    settings.beginWriteArray(QStringLiteral("docks"));
    int i = 0;
    const auto write = [&](const QString& name, int region) {
        settings.setArrayIndex(i++);
        settings.setValue(QStringLiteral("name"), name);
        settings.setValue(QStringLiteral("region"), region);
    };
    for (const auto& [dock, region] : attached) {
        write(dock->objectName(), region);
    }
    for (const auto& [name, region] : pending) {
        write(name, region);
    }
    settings.endArray();
}
