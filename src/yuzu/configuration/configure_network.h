// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <QWidget>

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

    std::unique_ptr<Ui::ConfigureNetwork> ui;

    const Core::System& system;
};
