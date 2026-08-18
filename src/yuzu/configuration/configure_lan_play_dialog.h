// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <QDialog>

class ConfigureLanPlay;

namespace Ui {
class ConfigureLanPlayDialog;
}

/// The LAN Play settings on their own, for the LAN Play entry in the menu bar.
class ConfigureLanPlayDialog : public QDialog {
    Q_OBJECT

public:
    explicit ConfigureLanPlayDialog(QWidget* parent);
    ~ConfigureLanPlayDialog() override;

    void ApplyConfiguration();

private:
    void changeEvent(QEvent* event) override;
    void RetranslateUI();

    std::unique_ptr<Ui::ConfigureLanPlayDialog> ui;

    ConfigureLanPlay* lan_play_widget;
};
