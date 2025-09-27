//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#include "Settings.h"

#include <mutex>
#include <QDir>
#include <boost/pfr.hpp>
#if __has_include(<magic_enum.hpp>)
#include <magic_enum.hpp>
#elif __has_include(<magic_enum/magic_enum.hpp>)
#include <magic_enum/magic_enum.hpp>
#else
#error "magic_enum header not found"
#endif

#include <Config.h>
#include "../Logger.h"
#include "../Application.h"
#include "GlobalMedia.h"
#include "LowAudioLatency.h"

using namespace boost;

namespace Core::Settings {

template <class T>
std::string_view LogSensitiveData(const T &value)
{
    return value != std::decay_t<T>{} ? "** MAYBE HAVE VALUE **" : "** MAYBE NO VALUE **";
}

void OnApply_language_locale(const Fields &newFields)
{
    LOG(Info, "OnApply_language_locale: {}", newFields.language_locale);

    ApdApp->SetTranslatorSafely(
        newFields.language_locale.isEmpty() ? QLocale{} : QLocale{newFields.language_locale});
}

void OnApply_auto_run(const Fields &newFields)
{
    LOG(Info, "OnApply_auto_run: {}", newFields.auto_run);

#if !defined APD_OS_WIN
    #error "Need to port."
#endif

    QSettings regAutoRun{
        "HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run",
        QSettings::Registry64Format};

    QString filePath = QDir::toNativeSeparators(ApdApplication::applicationFilePath());
    if (newFields.auto_run) {
        regAutoRun.setValue(Config::ProgramName, filePath);
    }
    else {
        regAutoRun.remove(Config::ProgramName);
    }
}

void OnApply_low_audio_latency(const Fields &newFields)
{
    LOG(Info, "OnApply_low_audio_latency: {}", newFields.low_audio_latency);

    ApdApp->GetLowAudioLatencyController()->ControlSafely(newFields.low_audio_latency);
}

void OnApply_automatic_ear_detection(const Fields &newFields)
{
    LOG(Info, "OnApply_automatic_ear_detection: {}", newFields.automatic_ear_detection);

    ApdApp->GetMainWindow()->GetApdMgr().OnAutomaticEarDetectionChanged(
        newFields.automatic_ear_detection);
}

void OnApply_rssi_min(const Fields &newFields)
{
    LOG(Info, "OnApply_rssi_min: {}", newFields.rssi_min);

    ApdApp->GetMainWindow()->GetApdMgr().OnRssiMinChanged(newFields.rssi_min);
}

void OnApply_device_address(const Fields &newFields)
{
    LOG(Info, "OnApply_device_address: {}", LogSensitiveData(newFields.device_address));

    if (newFields.device_address == 0) {
        ApdApp->GetMainWindow()->UnbindSafely();
    }
    else {
        ApdApp->GetMainWindow()->BindSafely();
    }

    ApdApp->GetMainWindow()->GetApdMgr().OnBoundDeviceAddressChanged(newFields.device_address);
}

void OnApply_tray_icon_battery(const Fields &newFields)
{
    LOG(Info, "OnApply_tray_icon_battery: {}", newFields.tray_icon_battery);

    ApdApp->GetTrayIcon()->OnTrayIconBatteryChangedSafely(newFields.tray_icon_battery);
}

void OnApply_battery_on_taskbar(const Fields &newFields)
{
    LOG(Info, "OnApply_battery_on_taskbar: {}", newFields.battery_on_taskbar);

    ApdApp->GetTaskbarStatus()->OnSettingsChangedSafely(newFields.battery_on_taskbar);
}

class Manager : public Helper::Singleton<Manager>
{
protected:
    Manager() = default;
    friend Helper::Singleton<Manager>;

public:
    LoadResult Load()
    {
        const auto &loadKey = [&](const std::string_view &keyName, auto &value,
                                  bool isSensitive = false) {
            using ValueType = std::decay_t<decltype(value)>;
            using ValueStorageType =
                std::conditional_t<!std::is_enum_v<ValueType>, ValueType, QString>;

            QString qstrKeyName = QString::fromStdString(std::string{keyName});
            if (!_settings.contains(qstrKeyName)) {
                if (!isSensitive) {
                    LOG(Warn, "The setting key '{}' not found. Current value '{}'.", keyName,
                        value);
                }
                else {
                    LOG(Warn, "The setting key '{}' not found. Current value '{}'.", keyName,
                        LogSensitiveData(value));
                }
                return false;
            }

            QVariant var = _settings.value(qstrKeyName);
            if (!var.canConvert<ValueStorageType>() ||
                !var.convert(qMetaTypeId<ValueStorageType>())) {
                LOG(Warn, "The value of the key '{}' cannot be convert.", keyName);
                return false;
            }

            if constexpr (!std::is_enum_v<ValueType>) {
                value = var.value<ValueStorageType>();
            }
            else {
                auto optValue =
                    magic_enum::enum_cast<ValueType>(var.value<ValueStorageType>().toStdString());
                if (!optValue.has_value()) {
                    LOG(Warn, "enum_cast the value of the key '{}' failed.", keyName);
                    return false;
                }
                value = optValue.value();
            }

            if (!isSensitive) {
                LOG(Info, "Load key succeeded. Key: '{}', Value: '{}'", keyName, value);
            }
            else {
                LOG(Info, "Load key succeeded. Key: '{}', Value: '{}'", keyName,
                    LogSensitiveData(value));
            }
            return true;
        };

        std::lock_guard<std::mutex> lock{_mutex};

        std::decay_t<decltype(kFieldsAbiVersion)> abi_version = 0;
        if (!loadKey("abi_version", abi_version)) {
            LOG(Warn, "No abi_version key. Load default settings.");
            _fields = Fields{};
            return LoadResult::NoAbiField;
        }
        else {
            if (abi_version != kFieldsAbiVersion) {
                LOG(Warn, "The settings abi version is incompatible. Local: '{}', Expect: '{}'",
                    abi_version, kFieldsAbiVersion);
                return LoadResult::AbiIncompatible;
            }

            pfr::for_each_field(_fieldsMeta, [&](auto &field) {
                loadKey(field.GetName(), field.GetValue(_fields), field.IsSensitive());
            });
            return LoadResult::Successful;
        }
    }

    void Save(Fields newFields)
    {
        std::lock_guard<std::mutex> lock{_mutex};

        _fields = std::move(newFields);
        SaveWithoutLock();
        ApplyWithoutLock();
    }

    void Apply()
    {
        std::lock_guard<std::mutex> lock{_mutex};

        ApplyWithoutLock();
    }

    Fields GetCurrent()
    {
        std::lock_guard<std::mutex> lock{_mutex};

        return _fields;
    }

    auto ConstAccess()
    {
        return ConstSafeAccessor{_mutex, _fields};
    }

    auto ModifiableAccess()
    {
        return ModifiableSafeAccessor{_mutex, _fields};
    }

private:
    MetaFields _fieldsMeta;

    std::mutex _mutex;
    Fields _fields;
    QSettings _settings{QSettings::UserScope, Config::ProgramName, Config::ProgramName};

    void SaveWithoutLock()
    {
        const auto &saveKey = [&]<class T>(
                                  const std::string_view &keyName, const T &value,
                                  bool isSensitive = false, bool isDeprecated = false) {
            QString qstrKeyName = QString::fromStdString(std::string{keyName});

            if (isDeprecated) {
                _settings.remove(qstrKeyName);
                LOG(Info, "Remove deprecated key succeeded. Key: '{}'", keyName);
                return;
            }

            if constexpr (!std::is_enum_v<T>) {
                _settings.setValue(qstrKeyName, value);
            }
            else {
                _settings.setValue(
                    qstrKeyName, QString::fromStdString(std::string{magic_enum::enum_name(value)}));
            }

            if (!isSensitive) {
                LOG(Info, "Save key succeeded. Key: '{}', Value: {}", keyName, value);
            }
            else {
                LOG(Info, "Save key succeeded. Key: '{}', Value: {}", keyName,
                    LogSensitiveData(value));
            }
        };

        saveKey("abi_version", kFieldsAbiVersion);

        pfr::for_each_field(_fieldsMeta, [&](const auto &fieldMeta) {
            saveKey(
                fieldMeta.GetName(), fieldMeta.GetValue(_fields), fieldMeta.IsSensitive(),
                fieldMeta.IsDeprecated());
        });
    }

    void ApplyWithoutLock()
    {
        LOG(Info, "ApplyWithoutLock");

        pfr::for_each_field(_fieldsMeta, [&](const auto &fieldMeta) {
            fieldMeta.OnApply().Invoke(std::cref(_fields));
        });
    }

    void ApplyChangedFieldsOnlyWithoutLock(const Fields &oldFields)
    {
        LOG(Info, "ApplyChangedFieldsOnlyWithoutLock");

        pfr::for_each_field(_fieldsMeta, [&](const auto &fieldMeta) {
            if (fieldMeta.GetValue(oldFields) != fieldMeta.GetValue(_fields)) {
                LOG(Info, "Changed field: {}", fieldMeta.GetName());
                fieldMeta.OnApply().Invoke(std::cref(_fields));
            }
        });
    }

    friend class ModifiableSafeAccessor;
};

ModifiableSafeAccessor::ModifiableSafeAccessor(std::mutex &lock, Fields &fields)
    : Impl::BasicSafeAccessor<Fields>{lock, fields}, _oldFields{fields}
{
}

ModifiableSafeAccessor::~ModifiableSafeAccessor()
{
    Manager::GetInstance().SaveWithoutLock();
    Manager::GetInstance().ApplyChangedFieldsOnlyWithoutLock(_oldFields);
}

LoadResult Load()
{
    return Manager::GetInstance().Load();
}

void Save(Fields newFields)
{
    return Manager::GetInstance().Save(std::move(newFields));
}

void Apply()
{
    return Manager::GetInstance().Apply();
}

Fields GetCurrent()
{
    return Manager::GetInstance().GetCurrent();
}

Fields GetDefault()
{

    return Fields{};
}

ConstSafeAccessor ConstAccess()
{
    return Manager::GetInstance().ConstAccess();
}

ModifiableSafeAccessor ModifiableAccess()
{
    return Manager::GetInstance().ModifiableAccess();
}
} // namespace Core::Settings
