/*
 * This file is part of Crystal Dock.
 * Copyright (C) 2022 Viet Dang (dangvd@gmail.com)
 *
 * Crystal Dock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Crystal Dock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Crystal Dock.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef CRYSTALDOCK_MEDIA_CONTROLS_H_
#define CRYSTALDOCK_MEDIA_CONTROLS_H_

#include "iconless_dock_item.h"

#include <QAction>
#include <QDBusInterface>
#include <QDBusServiceWatcher>
#include <QLabel>
#include <QMenu>
#include <QObject>
#include <QSlider>
#include <QString>
#include <QTimer>

namespace crystaldock {

// A media controls widget that integrates with MPRIS-compatible media players.
class MediaControls : public QObject, public IconlessDockItem {
  Q_OBJECT

 public:
  MediaControls(DockPanel* parent, MultiDockModel* model, Qt::Orientation orientation,
               int minSize, int maxSize);
  virtual ~MediaControls();

  void draw(QPainter* painter) const override;
  void mousePressEvent(QMouseEvent* e) override;
  QString getLabel() const override;
  bool beforeTask(const QString& program) override { return false; }

 public slots:
  void refreshMediaInfo();
  void onPlayPause();
  void onPrevious();
  void onNext();
  void onPositionSliderChanged(int value);
  void onPlayerSelected();
  void onDBusServiceRegistered(const QString& service);
  void onDBusServiceUnregistered(const QString& service);

 private:
  static constexpr float kWhRatio = 1.2;
  static constexpr int kUpdateInterval = 1000;

  enum class PlaybackStatus {
    Playing,
    Paused,
    Stopped
  };

  // Creates the context menu.
  void createMenu();

  // MPRIS player management.
  void updateAvailablePlayers();
  void connectToPlayer(const QString& service);
  void connectToBestPlayer();
  void checkForBetterPlayer();
  void disconnectFromPlayer();
  void updatePlayerInfo();
  void setPosition(int positionMs);

  // Converts MPRIS status string to enum.
  PlaybackStatus parsePlaybackStatus(const QString& status) const;

  // Gets user-friendly display name for a player service.
  QString getPlayerDisplayName(const QString& service) const;

  // Current player state.
  QString currentPlayer_;
  QString currentTitle_;
  QString currentArtist_;
  QString currentAlbum_;
  PlaybackStatus playbackStatus_ = PlaybackStatus::Stopped;
  int positionMs_ = 0;
  int durationMs_ = 0;
  bool hasPosition_ = false;

  // D-Bus interfaces.
  QDBusInterface* playerInterface_;
  QDBusInterface* propertiesInterface_;
  QDBusServiceWatcher* serviceWatcher_;

  // Update timer.
  QTimer* updateTimer_;

  // Context menu.
  QMenu menu_;
  QMenu* playerSelectionMenu_;
  QAction* playPauseAction_;
  QAction* previousAction_;
  QAction* nextAction_;
  QSlider* positionSlider_;
  QLabel* trackInfoLabel_;

  // Available players.
  QStringList availablePlayers_;
};

}  // namespace crystaldock

#endif  // CRYSTALDOCK_MEDIA_CONTROLS_H_