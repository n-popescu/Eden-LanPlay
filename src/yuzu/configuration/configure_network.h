// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <QFutureWatcher>
#include <QWidget>

#include "core/internal_network/lan_play/lan_play_connection_test.h"

namespace Ui {
class ConfigureNetwork;
}

class ConfigureNetwork : public QWidget {
    Q_OBJECT

public:
    explicit ConfigureNetwork(const Core::System& system_, QWidget* parent = nullptr);
    ~ConfigureNetwork() override;

    void ApplyConfiguration();

private:
    void changeEvent(QEvent*) override;
    void RetranslateUI();
    void SetConfiguration();

    /// Greys out the relay fields when LAN Play is off.
    void UpdateLanPlayEnabled(bool enabled);

    /// Joins the relay on a worker thread and reports what answered. See RunConnectionTest.
    void TestLanPlayConnection();

    std::unique_ptr<Ui::ConfigureNetwork> ui;

    /// The test blocks for several seconds, so it runs off the UI thread.
    QFutureWatcher<Network::LanPlay::ConnectionTestResult> lan_play_test_watcher;

    const Core::System& system;
};
