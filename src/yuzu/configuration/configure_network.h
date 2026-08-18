// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <QWidget>

namespace Ui {
class ConfigureNetwork;
}

namespace Core {
class System;
}

class ConfigureLanPlay;

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

    std::unique_ptr<Ui::ConfigureNetwork> ui;

    /// The same widget the LAN Play dialog in the menu bar shows.
    ConfigureLanPlay* lan_play_widget;

    const Core::System& system;
};
