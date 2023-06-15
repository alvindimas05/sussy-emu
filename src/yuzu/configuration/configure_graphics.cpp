// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <string>
#include <tuple>
#include <typeinfo>
#include <utility>
#include <vector>
#include <QBoxLayout>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QStringLiteral>
#include <QtCore/qobjectdefs.h>
#include <qabstractbutton.h>
#include <qboxlayout.h>
#include <qcoreevent.h>
#include <qglobal.h>
#include <qgridlayout.h>
#include <vulkan/vulkan_core.h>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_graphics.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_graphics.h"
#include "yuzu/configuration/shared_widget.h"
#include "yuzu/qt_common.h"
#include "yuzu/uisettings.h"
#include "yuzu/vk_device_info.h"

static const std::vector<VkPresentModeKHR> default_present_modes{VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                                 VK_PRESENT_MODE_FIFO_KHR};

// Converts a setting to a present mode (or vice versa)
static constexpr VkPresentModeKHR VSyncSettingToMode(Settings::VSyncMode mode) {
    switch (mode) {
    case Settings::VSyncMode::Immediate:
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    case Settings::VSyncMode::Mailbox:
        return VK_PRESENT_MODE_MAILBOX_KHR;
    case Settings::VSyncMode::Fifo:
        return VK_PRESENT_MODE_FIFO_KHR;
    case Settings::VSyncMode::FifoRelaxed:
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    default:
        return VK_PRESENT_MODE_FIFO_KHR;
    }
}

static constexpr Settings::VSyncMode PresentModeToSetting(VkPresentModeKHR mode) {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return Settings::VSyncMode::Immediate;
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return Settings::VSyncMode::Mailbox;
    case VK_PRESENT_MODE_FIFO_KHR:
        return Settings::VSyncMode::Fifo;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return Settings::VSyncMode::FifoRelaxed;
    default:
        return Settings::VSyncMode::Fifo;
    }
}

ConfigureGraphics::ConfigureGraphics(
    const Core::System& system_, std::vector<VkDeviceInfo::Record>& records_,
    const std::function<void()>& expose_compute_option_,
    std::shared_ptr<std::forward_list<ConfigurationShared::Tab*>> group,
    const ConfigurationShared::TranslationMap& translations_,
    const ConfigurationShared::ComboboxTranslationMap& combobox_translations_, QWidget* parent)
    : ConfigurationShared::Tab(group, parent), ui{std::make_unique<Ui::ConfigureGraphics>()},
      records{records_}, expose_compute_option{expose_compute_option_}, system{system_},
      translations{translations_}, combobox_translations{combobox_translations_},
      shader_mapping{combobox_translations.at(typeid(Settings::ShaderBackend))} {
    vulkan_device = Settings::values.vulkan_device.GetValue();
    RetrieveVulkanDevices();

    ui->setupUi(this);

    Setup();

    for (const auto& device : vulkan_devices) {
        vulkan_device_combobox->addItem(device);
    }

    UpdateBackgroundColorButton(QColor::fromRgb(Settings::values.bg_red.GetValue(),
                                                Settings::values.bg_green.GetValue(),
                                                Settings::values.bg_blue.GetValue()));
    UpdateAPILayout();
    PopulateVSyncModeSelection(); //< must happen after UpdateAPILayout

    // VSync setting needs to be determined after populating the VSync combobox
    if (Settings::IsConfiguringGlobal()) {
        const auto vsync_mode_setting = Settings::values.vsync_mode.GetValue();
        const auto vsync_mode = VSyncSettingToMode(vsync_mode_setting);
        int index{};
        for (const auto mode : vsync_mode_combobox_enum_map) {
            if (mode == vsync_mode) {
                break;
            }
            index++;
        }
        if (static_cast<unsigned long>(index) < vsync_mode_combobox_enum_map.size()) {
            vsync_mode_combobox->setCurrentIndex(index);
        }
    }

    connect(api_combobox, qOverload<int>(&QComboBox::activated), this, [this] {
        UpdateAPILayout();
        PopulateVSyncModeSelection();
    });
    connect(vulkan_device_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int device) {
                UpdateDeviceSelection(device);
                PopulateVSyncModeSelection();
            });
    connect(shader_backend_combobox, qOverload<int>(&QComboBox::activated), this,
            [this](int backend) { UpdateShaderBackendSelection(backend); });

    connect(ui->bg_button, &QPushButton::clicked, this, [this] {
        const QColor new_bg_color = QColorDialog::getColor(bg_color);
        if (!new_bg_color.isValid()) {
            return;
        }
        UpdateBackgroundColorButton(new_bg_color);
    });

    api_combobox->setEnabled(!UISettings::values.has_broken_vulkan && api_combobox->isEnabled());
    ui->api_widget->setEnabled(
        (!UISettings::values.has_broken_vulkan || Settings::IsConfiguringGlobal()) &&
        ui->api_widget->isEnabled());

    if (Settings::IsConfiguringGlobal()) {
        ui->bg_widget->setEnabled(Settings::values.bg_red.UsingGlobal());
    }
}

void ConfigureGraphics::PopulateVSyncModeSelection() {
    if (!Settings::IsConfiguringGlobal()) {
        return;
    }

    const Settings::RendererBackend backend{GetCurrentGraphicsBackend()};
    if (backend == Settings::RendererBackend::Null) {
        vsync_mode_combobox->setEnabled(false);
        return;
    }
    vsync_mode_combobox->setEnabled(true);

    const int current_index = //< current selected vsync mode from combobox
        vsync_mode_combobox->currentIndex();
    const auto current_mode = //< current selected vsync mode as a VkPresentModeKHR
        current_index == -1 ? VSyncSettingToMode(Settings::values.vsync_mode.GetValue())
                            : vsync_mode_combobox_enum_map[current_index];
    int index{};
    const int device{vulkan_device_combobox->currentIndex()}; //< current selected Vulkan device
    if (device == -1) {
        // Invalid device
        return;
    }

    const auto& present_modes = //< relevant vector of present modes for the selected device or API
        backend == Settings::RendererBackend::Vulkan ? device_present_modes[device]
                                                     : default_present_modes;

    vsync_mode_combobox->clear();
    vsync_mode_combobox_enum_map.clear();
    vsync_mode_combobox_enum_map.reserve(present_modes.size());
    for (const auto present_mode : present_modes) {
        const auto mode_name = TranslateVSyncMode(present_mode, backend);
        if (mode_name.isEmpty()) {
            continue;
        }

        vsync_mode_combobox->insertItem(index, mode_name);
        vsync_mode_combobox_enum_map.push_back(present_mode);
        if (present_mode == current_mode) {
            vsync_mode_combobox->setCurrentIndex(index);
        }
        index++;
    }
}

void ConfigureGraphics::UpdateDeviceSelection(int device) {
    if (device == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::Vulkan) {
        vulkan_device = device;
    }
}

void ConfigureGraphics::UpdateShaderBackendSelection(int backend) {
    if (backend == -1) {
        return;
    }
    if (GetCurrentGraphicsBackend() == Settings::RendererBackend::OpenGL) {
        shader_backend = static_cast<Settings::ShaderBackend>(backend);
    }
}

ConfigureGraphics::~ConfigureGraphics() = default;

void ConfigureGraphics::SetConfiguration() {}

void ConfigureGraphics::Setup() {
    const bool runtime_lock = !system.IsPoweredOn();
    QLayout* api_layout = ui->api_widget->layout();
    QWidget* api_grid_widget = new QWidget(this);
    QVBoxLayout* api_grid_layout = new QVBoxLayout(api_grid_widget);
    api_grid_layout->setContentsMargins(0, 0, 0, 0);
    api_layout->addWidget(api_grid_widget);

    QLayout& graphics_layout = *ui->graphics_widget->layout();

    std::map<u32, QWidget*> hold_graphics;
    std::forward_list<QWidget*> hold_api;

    for (const auto setting : Settings::values.linkage.by_category[Settings::Category::Renderer]) {
        if (!Settings::IsConfiguringGlobal() && !setting->Switchable()) {
            continue;
        }

        ConfigurationShared::Widget* widget = [&]() {
            // Set managed to false on these and set up the comboboxes ourselves
            if (setting->Id() == Settings::values.vulkan_device.Id() ||
                setting->Id() == Settings::values.shader_backend.Id() ||
                setting->Id() == Settings::values.vsync_mode.Id()) {
                return new ConfigurationShared::Widget(
                    setting, translations, combobox_translations, this, runtime_lock, apply_funcs,
                    ConfigurationShared::RequestType::ComboBox, false);
            } else if (setting->Id() == Settings::values.fsr_sharpening_slider.Id()) {
                // FSR needs a reversed slider
                return new ConfigurationShared::Widget(
                    setting, translations, combobox_translations, this, runtime_lock, apply_funcs,
                    ConfigurationShared::RequestType::ReverseSlider, true, 0.5f, nullptr,
                    tr("%1%", "FSR sharpening percentage (e.g. 50%)"));
            } else if (setting->Id() == Settings::values.speed_limit.Id()) {
                // speed_limit needs a checkbox to set use_speed_limit, as well as a spinbox
                return new ConfigurationShared::Widget(
                    setting, translations, combobox_translations, this, runtime_lock, apply_funcs,
                    &Settings::values.use_speed_limit, ConfigurationShared::RequestType::SpinBox,
                    tr("%", "Limit speed percentage (e.g. 50%)"));
            } else {
                return new ConfigurationShared::Widget(setting, translations, combobox_translations,
                                                       this, runtime_lock, apply_funcs);
            }
        }();

        if (!widget->Valid()) {
            delete widget;
            continue;
        }

        if (setting->Id() == Settings::values.renderer_backend.Id()) {
            // Add the renderer combobox now so it's at the top
            api_grid_layout->addWidget(widget);
            api_combobox = widget->combobox;
            api_restore_global_button = widget->restore_button;

            if (!Settings::IsConfiguringGlobal()) {
                QObject::connect(api_restore_global_button, &QAbstractButton::clicked,
                                 [=](bool) { UpdateAPILayout(); });

                // Detach API's restore button and place it where we want
                // Lets us put it on the side, and it will automatically scale if there's a
                // second combobox (shader_backend, vulkan_device)
                widget->layout()->removeWidget(api_restore_global_button);
                api_layout->addWidget(api_restore_global_button);
            }
        } else if (setting->Id() == Settings::values.vulkan_device.Id()) {
            // Keep track of vulkan_device's combobox so we can populate it
            hold_api.push_front(widget);
            vulkan_device_combobox = widget->combobox;
            vulkan_device_widget = widget;
        } else if (setting->Id() == Settings::values.shader_backend.Id()) {
            // Keep track of shader_backend's combobox so we can populate it
            hold_api.push_front(widget);
            shader_backend_combobox = widget->combobox;
            shader_backend_widget = widget;
        } else if (setting->Id() == Settings::values.vsync_mode.Id()) {
            // Keep track of vsync_mode's combobox so we can populate it
            vsync_mode_combobox = widget->combobox;
            hold_graphics.emplace(setting->Id(), widget);
        } else {
            hold_graphics.emplace(setting->Id(), widget);
        }
    }

    for (const auto& [id, widget] : hold_graphics) {
        graphics_layout.addWidget(widget);
    }

    for (auto widget : hold_api) {
        api_grid_layout->addWidget(widget);
    }

    // Background color is too specific to build into the new system, so we manage it here
    // (3 settings, all collected into a single widget with a QColor to manage on top)
    if (Settings::IsConfiguringGlobal()) {
        apply_funcs.push_front([this](bool powered_on) {
            Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
            Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
            Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
        });
    } else {
        QPushButton* bg_restore_button = ConfigurationShared::Widget::CreateRestoreGlobalButton(
            Settings::values.bg_red.UsingGlobal(), ui->bg_widget);
        ui->bg_widget->layout()->addWidget(bg_restore_button);

        QObject::connect(bg_restore_button, &QAbstractButton::clicked,
                         [bg_restore_button, this](bool) {
                             const int r = Settings::values.bg_red.GetValue(true);
                             const int g = Settings::values.bg_green.GetValue(true);
                             const int b = Settings::values.bg_blue.GetValue(true);
                             UpdateBackgroundColorButton(QColor::fromRgb(r, g, b));

                             bg_restore_button->setVisible(false);
                             bg_restore_button->setEnabled(false);
                         });

        QObject::connect(ui->bg_button, &QAbstractButton::clicked, [bg_restore_button](bool) {
            bg_restore_button->setVisible(true);
            bg_restore_button->setEnabled(true);
        });

        apply_funcs.push_front([bg_restore_button, this](bool powered_on) {
            const bool using_global = !bg_restore_button->isEnabled();
            Settings::values.bg_red.SetGlobal(using_global);
            Settings::values.bg_green.SetGlobal(using_global);
            Settings::values.bg_blue.SetGlobal(using_global);
            if (!using_global) {
                Settings::values.bg_red.SetValue(static_cast<u8>(bg_color.red()));
                Settings::values.bg_green.SetValue(static_cast<u8>(bg_color.green()));
                Settings::values.bg_blue.SetValue(static_cast<u8>(bg_color.blue()));
            }
        });
    }
}

const QString ConfigureGraphics::TranslateVSyncMode(VkPresentModeKHR mode,
                                                    Settings::RendererBackend backend) const {
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
        return backend == Settings::RendererBackend::OpenGL
                   ? tr("Off")
                   : QStringLiteral("Immediate (%1)").arg(tr("VSync Off"));
    case VK_PRESENT_MODE_MAILBOX_KHR:
        return QStringLiteral("Mailbox (%1)").arg(tr("Recommended"));
    case VK_PRESENT_MODE_FIFO_KHR:
        return backend == Settings::RendererBackend::OpenGL
                   ? tr("On")
                   : QStringLiteral("FIFO (%1)").arg(tr("VSync On"));
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return QStringLiteral("FIFO Relaxed");
    default:
        return {};
        break;
    }
}

int ConfigureGraphics::FindIndex(std::type_index enumeration, int value) const {
    for (u32 i = 0; i < combobox_translations.at(enumeration).size(); i++) {
        if (combobox_translations.at(enumeration)[i].first == static_cast<u32>(value)) {
            return i;
        }
    }
    return -1;
}

void ConfigureGraphics::ApplyConfiguration() {
    const bool powered_on = system.IsPoweredOn();
    for (const auto& func : apply_funcs) {
        func(powered_on);
    }

    if (Settings::IsConfiguringGlobal()) {
        const auto mode = vsync_mode_combobox_enum_map[vsync_mode_combobox->currentIndex()];
        const auto vsync_mode = PresentModeToSetting(mode);
        Settings::values.vsync_mode.SetValue(vsync_mode);
    }

    Settings::values.vulkan_device.SetGlobal(true);
    Settings::values.shader_backend.SetGlobal(true);
    if (Settings::IsConfiguringGlobal() ||
        (!Settings::IsConfiguringGlobal() && api_restore_global_button->isEnabled())) {
        auto backend = static_cast<Settings::RendererBackend>(
            combobox_translations
                .at(typeid(Settings::RendererBackend))[api_combobox->currentIndex()]
                .first);
        switch (backend) {
        case Settings::RendererBackend::OpenGL:
            Settings::values.shader_backend.SetGlobal(Settings::IsConfiguringGlobal());
            Settings::values.shader_backend.SetValue(static_cast<Settings::ShaderBackend>(
                shader_mapping[shader_backend_combobox->currentIndex()].first));
            break;
        case Settings::RendererBackend::Vulkan:
            Settings::values.vulkan_device.SetGlobal(Settings::IsConfiguringGlobal());
            Settings::values.vulkan_device.SetValue(vulkan_device_combobox->currentIndex());
            break;
        case Settings::RendererBackend::Null:
            break;
        }
    }
}

void ConfigureGraphics::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureGraphics::RetranslateUI() {
    ui->retranslateUi(this);
}

void ConfigureGraphics::UpdateBackgroundColorButton(QColor color) {
    bg_color = color;

    QPixmap pixmap(ui->bg_button->size());
    pixmap.fill(bg_color);

    const QIcon color_icon(pixmap);
    ui->bg_button->setIcon(color_icon);
}

void ConfigureGraphics::UpdateAPILayout() {
    bool runtime_lock = !system.IsPoweredOn();
    if (!Settings::IsConfiguringGlobal() && !api_restore_global_button->isEnabled()) {
        vulkan_device = Settings::values.vulkan_device.GetValue(true);
        shader_backend = Settings::values.shader_backend.GetValue(true);
        vulkan_device_widget->setEnabled(false);
        shader_backend_widget->setEnabled(false);
    } else {
        vulkan_device = Settings::values.vulkan_device.GetValue();
        shader_backend = Settings::values.shader_backend.GetValue();
        vulkan_device_widget->setEnabled(runtime_lock);
        shader_backend_widget->setEnabled(runtime_lock);
    }

    switch (GetCurrentGraphicsBackend()) {
    case Settings::RendererBackend::OpenGL:
        shader_backend_combobox->setCurrentIndex(
            FindIndex(typeid(Settings::ShaderBackend), static_cast<int>(shader_backend)));
        vulkan_device_widget->setVisible(false);
        shader_backend_widget->setVisible(true);
        break;
    case Settings::RendererBackend::Vulkan:
        if (static_cast<int>(vulkan_device) < vulkan_device_combobox->count()) {
            vulkan_device_combobox->setCurrentIndex(vulkan_device);
        }
        vulkan_device_widget->setVisible(true);
        shader_backend_widget->setVisible(false);
        break;
    case Settings::RendererBackend::Null:
        vulkan_device_widget->setVisible(false);
        shader_backend_widget->setVisible(false);
        break;
    }
}

void ConfigureGraphics::RetrieveVulkanDevices() {
    vulkan_devices.clear();
    vulkan_devices.reserve(records.size());
    device_present_modes.clear();
    device_present_modes.reserve(records.size());
    for (const auto& record : records) {
        vulkan_devices.push_back(QString::fromStdString(record.name));
        device_present_modes.push_back(record.vsync_support);

        if (record.has_broken_compute) {
            expose_compute_option();
        }
    }
}

Settings::RendererBackend ConfigureGraphics::GetCurrentGraphicsBackend() const {
    if (!Settings::IsConfiguringGlobal() && !api_restore_global_button->isEnabled()) {
        return Settings::values.renderer_backend.GetValue(true);
    }
    return static_cast<Settings::RendererBackend>(
        combobox_translations.at(typeid(Settings::RendererBackend))
            .at(api_combobox->currentIndex())
            .first);
}
