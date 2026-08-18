// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QFutureWatcher>
#include <QWidget>

#include "core/internal_network/lan_play/lan_play_connection_test.h"

namespace Ui {
class ConfigureLanPlay;
}

/**
 * The LAN Play settings. Lives both inside the Network page of the configuration dialog and in the
 * LAN Play dialog reached from the menu bar, so it is a widget rather than a dialog of its own.
 */
class ConfigureLanPlay : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureLanPlay(QWidget* parent = nullptr);
    ~ConfigureLanPlay() override;

    void ApplyConfiguration();

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();
    void SetConfiguration();

    /// Greys out the relay fields when LAN Play is off.
    void UpdateLanPlayEnabled(bool enabled);

    /// Joins the relay on a worker thread and reports what answered. See RunConnectionTest.
    void TestLanPlayConnection();

    std::unique_ptr<Ui::ConfigureLanPlay> ui;

    /// The test blocks for several seconds, so it runs off the UI thread.
    QFutureWatcher<Network::LanPlay::ConnectionTestResult> lan_play_test_watcher;
};
