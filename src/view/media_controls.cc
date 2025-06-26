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

#include "media_controls.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QIcon>
#include <QPainter>
#include <QWidgetAction>

#include "dock_panel.h"
#include <utils/draw_utils.h>

namespace crystaldock {

constexpr float MediaControls::kWhRatio;
constexpr int MediaControls::kUpdateInterval;

MediaControls::MediaControls(DockPanel* parent, MultiDockModel* model,
                           Qt::Orientation orientation, int minSize, int maxSize)
    : IconlessDockItem(parent, model, "Media Controls", orientation, minSize, maxSize,
                       kWhRatio),
      playerInterface_(nullptr),
      propertiesInterface_(nullptr),
      updateTimer_(new QTimer(this)) {
  
  // Set up D-Bus service watcher for MPRIS players
  serviceWatcher_ = new QDBusServiceWatcher(this);
  serviceWatcher_->setConnection(QDBusConnection::sessionBus());
  serviceWatcher_->setWatchMode(QDBusServiceWatcher::WatchForRegistration | 
                               QDBusServiceWatcher::WatchForUnregistration);
  serviceWatcher_->addWatchedService("org.mpris.MediaPlayer2.*");
  
  connect(serviceWatcher_, &QDBusServiceWatcher::serviceRegistered,
          this, &MediaControls::onDBusServiceRegistered);
  connect(serviceWatcher_, &QDBusServiceWatcher::serviceUnregistered,
          this, &MediaControls::onDBusServiceUnregistered);

  createMenu();

  // Set up update timer
  connect(updateTimer_, &QTimer::timeout, this, &MediaControls::refreshMediaInfo);
  updateTimer_->start(kUpdateInterval);

  // Initial scan for players
  updateAvailablePlayers();
  
  // Connect to best available player on startup
  if (!availablePlayers_.isEmpty()) {
    connectToBestPlayer();
  }

  connect(&menu_, &QMenu::aboutToHide, this,
          [this]() {
            parent_->setShowingPopup(false);
          });
}

MediaControls::~MediaControls() {
  disconnectFromPlayer();
}

void MediaControls::draw(QPainter* painter) const {
  const auto x = left_;
  const auto y = top_;
  const auto w = getWidth();
  const auto h = getHeight();

  // Try to use system icons first
  QString iconName;
  switch (playbackStatus_) {
    case PlaybackStatus::Playing:
      iconName = "media-playback-start";
      break;
    case PlaybackStatus::Paused:
      iconName = "media-playback-pause";
      break;
    case PlaybackStatus::Stopped:
    default:
      iconName = "media-playback-stop";
      break;
  }

  QIcon icon = QIcon::fromTheme(iconName);
  if (!icon.isNull()) {
    const int iconSize = qMin(w, h) * 0.8;
    const int iconX = x + (w - iconSize) / 2;
    const int iconY = y + (h - iconSize) / 2;
    icon.paint(painter, iconX, iconY, iconSize, iconSize);
  } else {
    // Fallback: draw custom media control icon
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(Qt::white, 2));
    painter->setBrush(Qt::white);

    const int centerX = x + w / 2;
    const int centerY = y + h / 2;
    const int iconSize = qMin(w, h) * 0.6;

    if (playbackStatus_ == PlaybackStatus::Playing) {
      // Draw pause symbol (two vertical bars)
      const int barWidth = iconSize / 6;
      const int barHeight = iconSize * 2 / 3;
      const int spacing = iconSize / 4;
      
      painter->fillRect(centerX - spacing/2 - barWidth, centerY - barHeight/2, 
                       barWidth, barHeight, Qt::white);
      painter->fillRect(centerX + spacing/2, centerY - barHeight/2, 
                       barWidth, barHeight, Qt::white);
    } else {
      // Draw play symbol (triangle pointing right)
      QPolygon playTriangle;
      const int triangleSize = iconSize / 2;
      playTriangle << QPoint(centerX - triangleSize/3, centerY - triangleSize/2)
                   << QPoint(centerX + triangleSize*2/3, centerY)
                   << QPoint(centerX - triangleSize/3, centerY + triangleSize/2);
      painter->drawPolygon(playTriangle);
    }

    // Draw small indicator if player is available
    if (!currentPlayer_.isEmpty()) {
      painter->setPen(QPen(Qt::green, 1));
      painter->setBrush(Qt::green);
      painter->drawEllipse(centerX + iconSize/3, centerY - iconSize/3, 4, 4);
    }
  }
}

void MediaControls::mousePressEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton) {
    if (currentPlayer_.isEmpty()) {
      // No player available, show menu to select
      showPopupMenu(&menu_);
    } else {
      // Quick play/pause toggle
      onPlayPause();
    }
  } else if (e->button() == Qt::RightButton) {
    showPopupMenu(&menu_);
  } else if (e->button() == Qt::MiddleButton) {
    onNext();
  }
}

QString MediaControls::getLabel() const {
  if (currentPlayer_.isEmpty()) {
    return "Media Controls: No player";
  }
  
  if (!currentTitle_.isEmpty()) {
    QString label = currentTitle_;
    if (!currentArtist_.isEmpty()) {
      label += " - " + currentArtist_;
    }
    return label;
  }
  
  return "Media Controls: " + getPlayerDisplayName(currentPlayer_);
}

void MediaControls::refreshMediaInfo() {
  if (!playerInterface_) {
    return;
  }

  updatePlayerInfo();
  
  // Check if we should switch to a better player (one that started playing)
  if (availablePlayers_.size() > 1 && playbackStatus_ != PlaybackStatus::Playing) {
    checkForBetterPlayer();
  }
}

void MediaControls::onPlayPause() {
  if (!playerInterface_) {
    return;
  }

  if (playbackStatus_ == PlaybackStatus::Playing) {
    playerInterface_->call("Pause");
  } else {
    playerInterface_->call("Play");
  }
}

void MediaControls::onPrevious() {
  if (!playerInterface_) {
    return;
  }
  playerInterface_->call("Previous");
}

void MediaControls::onNext() {
  if (!playerInterface_) {
    return;
  }
  playerInterface_->call("Next");
}

void MediaControls::onPositionSliderChanged(int value) {
  if (!playerInterface_ || !hasPosition_) {
    return;
  }
  
  // Convert percentage to microseconds (MPRIS uses microseconds)
  qint64 positionUs = (static_cast<qint64>(value) * durationMs_ * 1000) / 100;
  setPosition(positionUs / 1000);
}

void MediaControls::onPlayerSelected() {
  QAction* action = qobject_cast<QAction*>(sender());
  if (!action) {
    return;
  }

  QString service = action->data().toString();
  connectToPlayer(service);
}

void MediaControls::onDBusServiceRegistered(const QString& service) {
  if (service.startsWith("org.mpris.MediaPlayer2.")) {
    updateAvailablePlayers();
    
    // Smart auto-connect logic
    if (currentPlayer_.isEmpty() && !availablePlayers_.isEmpty()) {
      connectToBestPlayer();
    }
  }
}

void MediaControls::onDBusServiceUnregistered(const QString& service) {
  if (service.startsWith("org.mpris.MediaPlayer2.")) {
    if (service == currentPlayer_) {
      disconnectFromPlayer();
      // Try to connect to the best available player
      updateAvailablePlayers();
      if (!availablePlayers_.isEmpty()) {
        connectToBestPlayer();
      }
    }
    updateAvailablePlayers();
  }
}

void MediaControls::createMenu() {
  // Player selection submenu
  playerSelectionMenu_ = menu_.addMenu("Select Player");
  
  menu_.addSeparator();

  // Track info label
  trackInfoLabel_ = new QLabel("No media playing");
  trackInfoLabel_->setAlignment(Qt::AlignCenter);
  trackInfoLabel_->setMinimumWidth(200);
  QWidgetAction* trackInfoAction = new QWidgetAction(&menu_);
  trackInfoAction->setDefaultWidget(trackInfoLabel_);
  menu_.addAction(trackInfoAction);

  // Position slider
  positionSlider_ = new QSlider(Qt::Horizontal);
  positionSlider_->setRange(0, 100);
  positionSlider_->setValue(0);
  positionSlider_->setMinimumWidth(200);
  positionSlider_->setEnabled(false);
  connect(positionSlider_, &QSlider::valueChanged, this, &MediaControls::onPositionSliderChanged);

  QWidgetAction* sliderAction = new QWidgetAction(&menu_);
  sliderAction->setDefaultWidget(positionSlider_);
  menu_.addAction(sliderAction);

  menu_.addSeparator();

  // Media control actions
  previousAction_ = menu_.addAction("Previous", this, &MediaControls::onPrevious);
  playPauseAction_ = menu_.addAction("Play", this, &MediaControls::onPlayPause);
  nextAction_ = menu_.addAction("Next", this, &MediaControls::onNext);

  // Initially disable controls
  previousAction_->setEnabled(false);
  playPauseAction_->setEnabled(false);
  nextAction_->setEnabled(false);

  menu_.addSeparator();
  parent_->addPanelSettings(&menu_);
}

void MediaControls::updateAvailablePlayers() {
  availablePlayers_.clear();
  
  QDBusConnectionInterface* interface = QDBusConnection::sessionBus().interface();
  if (!interface) {
    return;
  }

  QDBusReply<QStringList> reply = interface->registeredServiceNames();
  if (!reply.isValid()) {
    return;
  }

  for (const QString& service : reply.value()) {
    if (service.startsWith("org.mpris.MediaPlayer2.")) {
      availablePlayers_.append(service);
    }
  }

  // Update player selection menu
  playerSelectionMenu_->clear();
  for (const QString& service : availablePlayers_) {
    QString displayName = getPlayerDisplayName(service);
    QAction* action = playerSelectionMenu_->addAction(displayName);
    action->setData(service);
    action->setCheckable(true);
    action->setChecked(service == currentPlayer_);
    connect(action, &QAction::triggered, this, &MediaControls::onPlayerSelected);
  }

  if (availablePlayers_.isEmpty()) {
    playerSelectionMenu_->addAction("No players available")->setEnabled(false);
  }
}

void MediaControls::connectToBestPlayer() {
  if (availablePlayers_.isEmpty()) {
    return;
  }

  // If only one player, use it
  if (availablePlayers_.size() == 1) {
    connectToPlayer(availablePlayers_.first());
    return;
  }

  // Multiple players - find the best one to connect to
  QString bestPlayer;
  PlaybackStatus bestStatus = PlaybackStatus::Stopped;
  
  for (const QString& service : availablePlayers_) {
    // Create temporary interfaces to check player status
    QDBusInterface tempPropertiesInterface(service, "/org/mpris/MediaPlayer2",
                                          "org.freedesktop.DBus.Properties",
                                          QDBusConnection::sessionBus());
    
    if (!tempPropertiesInterface.isValid()) {
      continue;
    }

    // Get playback status
    QDBusReply<QVariant> statusReply = tempPropertiesInterface.call("Get", 
        "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
    
    if (!statusReply.isValid()) {
      continue;
    }

    PlaybackStatus status = parsePlaybackStatus(statusReply.value().toString());
    
    // Priority: Playing > Paused > Stopped
    if (status == PlaybackStatus::Playing) {
      // Found a playing player - use it immediately
      connectToPlayer(service);
      return;
    } else if (status == PlaybackStatus::Paused && bestStatus == PlaybackStatus::Stopped) {
      // Prefer paused over stopped
      bestPlayer = service;
      bestStatus = status;
    } else if (bestPlayer.isEmpty()) {
      // First player we found
      bestPlayer = service;
      bestStatus = status;
    }
  }

  // Connect to the best player we found
  if (!bestPlayer.isEmpty()) {
    connectToPlayer(bestPlayer);
  }
}

void MediaControls::checkForBetterPlayer() {
  // Only check for better players if current one is not playing
  if (playbackStatus_ == PlaybackStatus::Playing) {
    return;
  }

  for (const QString& service : availablePlayers_) {
    // Skip current player
    if (service == currentPlayer_) {
      continue;
    }

    // Create temporary interface to check player status
    QDBusInterface tempPropertiesInterface(service, "/org/mpris/MediaPlayer2",
                                          "org.freedesktop.DBus.Properties",
                                          QDBusConnection::sessionBus());
    
    if (!tempPropertiesInterface.isValid()) {
      continue;
    }

    // Get playback status
    QDBusReply<QVariant> statusReply = tempPropertiesInterface.call("Get", 
        "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
    
    if (!statusReply.isValid()) {
      continue;
    }

    PlaybackStatus status = parsePlaybackStatus(statusReply.value().toString());
    
    // If we find a playing player, switch to it
    if (status == PlaybackStatus::Playing) {
      connectToPlayer(service);
      return;
    }
  }
}

void MediaControls::connectToPlayer(const QString& service) {
  disconnectFromPlayer();

  currentPlayer_ = service;
  
  // Create player interface
  playerInterface_ = new QDBusInterface(service, "/org/mpris/MediaPlayer2",
                                       "org.mpris.MediaPlayer2.Player",
                                       QDBusConnection::sessionBus(), this);

  // Create properties interface for monitoring changes
  propertiesInterface_ = new QDBusInterface(service, "/org/mpris/MediaPlayer2",
                                           "org.freedesktop.DBus.Properties",
                                           QDBusConnection::sessionBus(), this);

  if (!playerInterface_->isValid()) {
    disconnectFromPlayer();
    return;
  }

  // Enable controls
  previousAction_->setEnabled(true);
  playPauseAction_->setEnabled(true);
  nextAction_->setEnabled(true);

  // Update player selection menu
  updateAvailablePlayers();
  
  // Get initial state
  updatePlayerInfo();
  
  parent_->update();
}

void MediaControls::disconnectFromPlayer() {
  if (playerInterface_) {
    playerInterface_->deleteLater();
    playerInterface_ = nullptr;
  }

  if (propertiesInterface_) {
    propertiesInterface_->deleteLater();
    propertiesInterface_ = nullptr;
  }

  currentPlayer_.clear();
  currentTitle_.clear();
  currentArtist_.clear();
  currentAlbum_.clear();
  playbackStatus_ = PlaybackStatus::Stopped;
  positionMs_ = 0;
  durationMs_ = 0;
  hasPosition_ = false;

  // Disable controls
  previousAction_->setEnabled(false);
  playPauseAction_->setEnabled(false);
  nextAction_->setEnabled(false);
  positionSlider_->setEnabled(false);
  
  trackInfoLabel_->setText("No media playing");
  
  parent_->update();
}

void MediaControls::updatePlayerInfo() {
  if (!playerInterface_ || !propertiesInterface_) {
    return;
  }

  // Get playback status
  QDBusReply<QVariant> statusReply = propertiesInterface_->call("Get", 
      "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
  if (statusReply.isValid()) {
    playbackStatus_ = parsePlaybackStatus(statusReply.value().toString());
    playPauseAction_->setText(playbackStatus_ == PlaybackStatus::Playing ? "Pause" : "Play");
  }

  // Get metadata
  QDBusReply<QVariant> metadataReply = propertiesInterface_->call("Get",
      "org.mpris.MediaPlayer2.Player", "Metadata");
  if (metadataReply.isValid()) {
    QVariantMap metadata = metadataReply.value().toMap();
    
    currentTitle_ = metadata.value("xesam:title").toString();
    currentArtist_ = metadata.value("xesam:artist").toStringList().join(", ");
    currentAlbum_ = metadata.value("xesam:album").toString();
    
    // Duration in microseconds, convert to milliseconds
    qint64 durationUs = metadata.value("mpris:length").toLongLong();
    durationMs_ = durationUs / 1000;
    
    hasPosition_ = (durationMs_ > 0);
    positionSlider_->setEnabled(hasPosition_);
  }

  // Get position if available
  if (hasPosition_) {
    QDBusReply<QVariant> positionReply = propertiesInterface_->call("Get",
        "org.mpris.MediaPlayer2.Player", "Position");
    if (positionReply.isValid()) {
      qint64 positionUs = positionReply.value().toLongLong();
      positionMs_ = positionUs / 1000;
      
      // Update slider without triggering signal
      positionSlider_->blockSignals(true);
      if (durationMs_ > 0) {
        int percentage = (positionMs_ * 100) / durationMs_;
        positionSlider_->setValue(percentage);
      }
      positionSlider_->blockSignals(false);
    }
  }

  // Update track info label
  QString trackInfo = "No media playing";
  if (!currentTitle_.isEmpty()) {
    trackInfo = currentTitle_;
    if (!currentArtist_.isEmpty()) {
      trackInfo += "\n" + currentArtist_;
    }
  }
  trackInfoLabel_->setText(trackInfo);

  parent_->update();
}

void MediaControls::setPosition(int positionMs) {
  if (!playerInterface_) {
    return;
  }
  
  // MPRIS SetPosition takes track ID and position in microseconds
  QDBusReply<QVariant> trackIdReply = propertiesInterface_->call("Get",
      "org.mpris.MediaPlayer2.Player", "Metadata");
  if (trackIdReply.isValid()) {
    QVariantMap metadata = trackIdReply.value().toMap();
    QDBusObjectPath trackId = metadata.value("mpris:trackid").value<QDBusObjectPath>();
    
    qint64 positionUs = static_cast<qint64>(positionMs) * 1000;
    playerInterface_->call("SetPosition", QVariant::fromValue(trackId), positionUs);
  }
}

MediaControls::PlaybackStatus MediaControls::parsePlaybackStatus(const QString& status) const {
  if (status == "Playing") {
    return PlaybackStatus::Playing;
  } else if (status == "Paused") {
    return PlaybackStatus::Paused;
  } else {
    return PlaybackStatus::Stopped;
  }
}

QString MediaControls::getPlayerDisplayName(const QString& service) const {
  // Try to get the Identity property from the MediaPlayer2 interface
  QDBusInterface playerInterface(service, "/org/mpris/MediaPlayer2",
                                "org.freedesktop.DBus.Properties",
                                QDBusConnection::sessionBus());
  
  if (playerInterface.isValid()) {
    QDBusReply<QVariant> identityReply = playerInterface.call("Get",
        "org.mpris.MediaPlayer2", "Identity");
    
    if (identityReply.isValid()) {
      QString identity = identityReply.value().toString();
      if (!identity.isEmpty()) {
        return identity;
      }
    }
    
    // Try getting DesktopEntry for fallback
    QDBusReply<QVariant> desktopReply = playerInterface.call("Get",
        "org.mpris.MediaPlayer2", "DesktopEntry");
    
    if (desktopReply.isValid()) {
      QString desktop = desktopReply.value().toString();
      if (!desktop.isEmpty()) {
        // Capitalize first letter and return
        return desktop.left(1).toUpper() + desktop.mid(1);
      }
    }
  }
  
  // Fallback to parsing service name
  QString displayName = service;
  displayName.remove("org.mpris.MediaPlayer2.");
  
  // Handle common patterns
  if (displayName.startsWith("firefox.instance")) {
    return "Firefox";
  } else if (displayName.startsWith("chromium.instance")) {
    return "Chromium";
  } else if (displayName.startsWith("chrome.instance")) {
    return "Chrome";
  } else if (displayName.startsWith("spotify.instance")) {
    return "Spotify";
  } else if (displayName.startsWith("vlc.instance")) {
    return "VLC";
  } else if (displayName.contains(".instance")) {
    // Generic instance pattern - extract base name
    QString baseName = displayName.split(".instance").first();
    return baseName.left(1).toUpper() + baseName.mid(1);
  }
  
  // Final fallback - capitalize first letter
  return displayName.left(1).toUpper() + displayName.mid(1);
}

}  // namespace crystaldock