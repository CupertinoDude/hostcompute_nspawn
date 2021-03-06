/*
 * Copyright 2016, akashche at redhat.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hostcompute_nspawn.h"

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <iostream>

#include "staticlib/config.hpp"
#include "staticlib/support.hpp"
#include "staticlib/io.hpp"
#include "staticlib/json.hpp"
#include "staticlib/ranges.hpp"
#include "staticlib/tinydir.hpp"
#include "staticlib/utils.hpp"

#include "callback_latch.hpp"
#include "container_config.hpp"
#include "container_layer.hpp"
#include "notification_type.hpp"
#include "nspawn_config.hpp"
#include "nspawn_exception.hpp"
#include "process_config.hpp"

namespace nspawn {

enum class HcsErrors : uint32_t {
    operation_pending = 0xC0370103
};

DriverInfo create_driver_info(const std::wstring& wide_base_path) {
    DriverInfo res;
    std::memset(std::addressof(res), '\0', sizeof(DriverInfo));
    res.Flavour = GraphDriverType::FilterDriver;
    res.HomeDir = wide_base_path.c_str();
    return res;
}

std::vector<container_layer> collect_acsendant_layers(const std::string& base_path,
        const std::string& parent_layer_name) {
    std::vector<container_layer> res;
    res.emplace_back(base_path, parent_layer_name);
    auto json_file = std::string(base_path) + "\\" + parent_layer_name + "\\layerchain.json";
    auto src = sl::tinydir::file_source(json_file);
    auto json = sl::json::load(src);
    std::cout << "Ascendant layers: " << json.dumps() << std::endl;
    for (auto& el : json.as_array_or_throw(json_file)) {
        std::string path = el.as_string_or_throw(json_file);
        std::string dir = sl::utils::strip_filename(path);
        std::string file = sl::utils::strip_parent_dir(path);
        res.emplace_back(dir, file);
    }
    return res;
}

std::vector<WC_LAYER_DESCRIPTOR> create_ascendant_descriptors(const std::vector<container_layer>& acsendant_layers) {
    auto ra = sl::ranges::transform(acsendant_layers, [](const container_layer& la){
        return la.to_descriptor();
    });
    return ra.to_vector();
}

void hcs_create_layer(DriverInfo& driver_info, container_layer& layer, const std::string& parent_layer_name, 
        std::vector<WC_LAYER_DESCRIPTOR>& acsendant_descriptors) {
    std::wstring wname = sl::utils::widen(layer.get_name());
    std::wstring wparent = sl::utils::widen(parent_layer_name);
    auto err = ::CreateSandboxLayer(std::addressof(driver_info), wname.c_str(), wparent.c_str(),
            acsendant_descriptors.data(), static_cast<uint32_t>(acsendant_descriptors.size()));
    if (0 != err) {
        throw nspawn_exception(TRACEMSG("'CreateSandboxLayer' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " parent_layer_name: [" + parent_layer_name + "]," +
                " error: [" + sl::utils::errcode_to_string(err) + "]"));
    }
    std::cout << "CreateSandboxLayer: " << "Layer created, name: [" << layer.get_name() << "]" << std::endl;
}

void hcs_activate_layer(DriverInfo& driver_info, container_layer& layer) {
    std::wstring wname = sl::utils::widen(layer.get_name());
    auto err = ::ActivateLayer(std::addressof(driver_info), wname.c_str());
    if (0 != err) {
        throw nspawn_exception(TRACEMSG("'ActivateLayer' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " error: [" + sl::utils::errcode_to_string(err) + "]"));
    }
    std::cout << "ActivateLayer: " << "Layer activated, name: [" << layer.get_name() << "]" << std::endl;
}

void hcs_prepare_layer(DriverInfo& driver_info, container_layer& layer, 
        std::vector<WC_LAYER_DESCRIPTOR>& acsendant_descriptors) {
    std::wstring wname = sl::utils::widen(layer.get_name());
    auto err = ::PrepareLayer(std::addressof(driver_info), wname.c_str(),
        acsendant_descriptors.data(), static_cast<uint32_t>(acsendant_descriptors.size()));
    if (0 != err) {
        throw nspawn_exception(TRACEMSG("'PrepareLayer' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " error: [" + sl::utils::errcode_to_string(err) + "]"));
    }
    std::cout << "PrepareLayer: " << "Layer prepared, name: [" << layer.get_name() << "]" << std::endl;
}

std::string hcs_get_layer_mount_path(DriverInfo& driver_info, container_layer& layer) {
    std::wstring wname = sl::utils::widen(layer.get_name());
    std::wstring path;
    path.resize(MAX_PATH);
    uint32_t length = MAX_PATH;
    auto err = ::GetLayerMountPath(std::addressof(driver_info), wname.c_str(),
        std::addressof(length), std::addressof(path.front()));
    if (0 != err) {
        throw nspawn_exception(TRACEMSG("'GetLayerMountPath' failed," +
                " layer_name: [" + layer.get_name() + "]," +
                " error: [" + sl::utils::errcode_to_string(err) + "]"));
    }
    std::string res = sl::utils::narrow(path.c_str());
    std::cout << "GetLayerMountPath: " << "Found volume path: [" << res << "]" <<
            " for layer, name: [" << layer.get_name() << "]" << std::endl;
    return res;
}

HANDLE hcs_create_compute_system(container_config& config, container_layer& layer) {
    std::wstring wname = sl::utils::widen(layer.get_name());
    std::string conf = config.to_json().dumps();
    std::wstring wconf = sl::utils::widen(conf);
    HANDLE identity = nullptr;
    HANDLE computeSystem = nullptr;
    wchar_t* result = nullptr;
    auto res = ::HcsCreateComputeSystem(wname.c_str(), wconf.c_str(), identity,
        std::addressof(computeSystem), std::addressof(result));
    if (static_cast<uint32_t>(HcsErrors::operation_pending) != res) {
        throw nspawn_exception(TRACEMSG("'HcsCreateComputeSystem' failed," +
                " config: [" + conf + "]," +
                " error: [" + sl::utils::errcode_to_string(res) + "]"));
    }
    std::cout << "HcsCreateComputeSystem: " << "Container created, name: [" << layer.get_name() << "]" << std::endl;
    return computeSystem;
}

void container_callback(uint32_t notificationType, void* context, int32_t notificationStatus,
        wchar_t* notificationData) STATICLIB_NOEXCEPT {
    std::string data = nullptr != notificationData ? sl::utils::narrow(notificationData) : "";
    std::cout << "CS notification received, notificationType: [" << sl::support::to_string(notificationType) << "]," <<
            " notificationStatus: [" << notificationStatus << "]," <<
            " notificationData: [" << data << "]" << std::endl;
    callback_latch& la = *static_cast<callback_latch*> (context);
    la.unlock(static_cast<notification_type>(notificationType));
};

HANDLE hcs_register_compute_system_callback(HANDLE compute_system, container_layer& layer,
        callback_latch& latch) {
    HANDLE cs_callback_handle = nullptr;
    latch.lock();
    auto res = ::HcsRegisterComputeSystemCallback(compute_system, container_callback, static_cast<void*>(std::addressof(latch)),
        std::addressof(cs_callback_handle));
    if (0 != res) {
        latch.cancel();
        throw nspawn_exception(TRACEMSG("'HcsRegisterComputeSystemCallback' failed," +
                " name: [" + layer.get_name() + "]," +
                " error: [" + sl::utils::errcode_to_string(res) + "]"));
    }
    std::cout << "HcsRegisterComputeSystemCallback: " << "CS callback registered successfully, name: [" << layer.get_name() << "]" << std::endl;
    latch.await(notification_type::system_create_complete);
    std::cout << "HcsRegisterComputeSystemCallback: " << "CS create latch unlocked" << std::endl;
    return cs_callback_handle;
}

void hcs_start_compute_system(HANDLE compute_system, container_layer& layer, callback_latch& latch) {
    std::wstring options = sl::utils::widen("");
    wchar_t* result = nullptr;
    latch.lock();
    auto res = ::HcsStartComputeSystem(compute_system, options.c_str(), std::addressof(result));
    if (static_cast<uint32_t>(HcsErrors::operation_pending) != res) {
        latch.cancel();
        throw nspawn_exception(TRACEMSG("'HcsStartComputeSystem' failed," +
                " error: [" + sl::utils::errcode_to_string(res) + "]"));
    }
    latch.await(notification_type::system_start_complete);
    std::cout << "HcsStartComputeSystem: " << "Container started, name: [" << layer.get_name() << "]" << std::endl;
}

void hcs_enumerate_compute_systems() {
    std::wstring query = sl::utils::widen("{}");
    wchar_t* computeSystems = nullptr;
    wchar_t* result = nullptr;
    auto res = ::HcsEnumerateComputeSystems(query.c_str(),
            std::addressof(computeSystems), std::addressof(result));
    if (0 != res) {
        throw nspawn_exception(TRACEMSG("'HcsEnumerateComputeSystems' failed," +
                " error: [" + sl::utils::errcode_to_string(res) + "]"));
    }
    std::cout << "HcsEnumerateComputeSystems: " << "Compute systems found: " << sl::utils::narrow(computeSystems) << std::endl;
}

HANDLE hcs_create_process(HANDLE compute_system, const nspawn_config& config) {
    HANDLE process = nullptr;
    auto pcfg = process_config(config);
    std::string pcfg_json = pcfg.to_json().dumps();
    std::cout << "Process config: " << pcfg_json << std::endl;
    std::wstring wpcfg_json = sl::utils::widen(pcfg_json);
    HCS_PROCESS_INFORMATION hpi;
    std::memset(std::addressof(hpi), '\0', sizeof(HCS_PROCESS_INFORMATION));
    wchar_t* result = nullptr;
    auto res = ::HcsCreateProcess(compute_system, wpcfg_json.c_str(), std::addressof(hpi),
        std::addressof(process), std::addressof(result));
    if (0 != res) {
        throw nspawn_exception(TRACEMSG("'HcsCreateProcess' failed," +
                " config: [" + pcfg_json + "]," +
                " error: [" + sl::utils::errcode_to_string(res) + "]"));
    }
    std::cout << "HcsCreateProcess: " << "Process created" << std::endl;
    return process;
}

HANDLE hcs_register_process_callback(HANDLE process, container_layer& layer, callback_latch& latch) {
    HANDLE process_callback_handle;
    latch.lock();
    auto res = ::HcsRegisterProcessCallback(process, container_callback, std::addressof(latch), std::addressof(process_callback_handle));
    if (0 != res) {
        latch.cancel();
        throw nspawn_exception(TRACEMSG("'HcsRegisterProcessCallback' failed," +
                " name: [" + layer.get_name() + "]," +
                " error: [" + sl::utils::errcode_to_string(res) + "]"));
    }
    std::cout << "HcsRegisterProcessCallback: " << "Process callback registered successfully, name: [" << layer.get_name() << "]" << std::endl;
    latch.await(notification_type::process_exit);
    std::cout << "HcsRegisterProcessCallback: " << "Process create latch unlocked" << std::endl;
    return process_callback_handle;
}

void hcs_terminate_compute_system(HANDLE compute_system, container_layer& layer, callback_latch& latch) STATICLIB_NOEXCEPT {
    std::wstring options = sl::utils::widen("{}");
    wchar_t* result = nullptr;
    latch.lock();
    auto res = ::HcsTerminateComputeSystem(compute_system, options.c_str(), std::addressof(result));
    if (static_cast<uint32_t>(HcsErrors::operation_pending) == res) {
        latch.await(notification_type::system_exit);
        std::cout << "HcsTerminateComputeSystem: " << "Container terminated, name: [" << layer.get_name() << "]" << std::endl;
    }
    else {
        latch.cancel();
        std::cerr << "ERROR: 'HcsTerminateComputeSystem' failed, name: [" << layer.get_name() << "]" <<
            " error: [" << sl::utils::errcode_to_string(res) << "]" << std::endl;
    }
}

void hcs_unprepare_layer(DriverInfo& driver_info, container_layer& layer) STATICLIB_NOEXCEPT {
    std::wstring wname = sl::utils::widen(layer.get_name());
    auto res = ::UnprepareLayer(std::addressof(driver_info), wname.c_str());
    if (0 == res) {
        std::cout << "UnprepareLayer: " << "Layer unprepared, name: [" << layer.get_name() << "]" << std::endl;
    }
    else {
        std::cerr << "ERROR: 'UnprepareLayer' failed, name: [" << layer.get_name() << "]" <<
            " error: [" << sl::utils::errcode_to_string(res) << "]" << std::endl;
    }
}

void hcs_deactivate_layer(DriverInfo& driver_info, container_layer& layer) STATICLIB_NOEXCEPT {
    std::wstring wname = sl::utils::widen(layer.get_name());
    auto res = ::DeactivateLayer(std::addressof(driver_info), wname.c_str());
    if (0 == res) {
        std::cout << "DeactivateLayer: " << "Layer deactivated, name: [" << layer.get_name() << "]" << std::endl;
    }
    else {
        std::cerr << "ERROR: 'DeactivateLayer' failed, name: [" << layer.get_name() << "]" <<
            " error: [" << sl::utils::errcode_to_string(res) << "]" << std::endl;
    }
}

void hcs_destroy_layer(DriverInfo& driver_info, container_layer& layer) STATICLIB_NOEXCEPT {
    std::wstring wname = sl::utils::widen(layer.get_name());
    auto res = ::DestroyLayer(std::addressof(driver_info), wname.c_str());
    if (0 == res) {
        std::cout << "DestroyLayer: " << "Layer destroyed, name: [" << layer.get_name() << "]" << std::endl;
    }
    else {
        std::cerr << "ERROR: 'DestroyLayer' failed, name: [" << layer.get_name() << "]" <<
            " error: [" << sl::utils::errcode_to_string(res) << "]" << std::endl;
    }
}

void spawn_and_wait(const nspawn_config& config) {
    std::cout << "nspawn config: " << config.to_json().dumps() << std::endl;

    // common parameters
    auto rng = sl::utils::random_string_generator("0123456789abcdef");
    std::string base_path = sl::utils::strip_filename(config.parent_layer_directory);
    std::wstring wide_base_path = sl::utils::widen(base_path);
    std::string parent_layer_name = sl::utils::strip_parent_dir(config.parent_layer_directory);

    // prepare DriverInfo
    DriverInfo driver_info = create_driver_info(wide_base_path);

    // prepare acsendants
    auto acsendant_layers = collect_acsendant_layers(base_path, parent_layer_name);
    auto acsendant_descriptors = create_ascendant_descriptors(acsendant_layers);

    // create layer
    auto layer = container_layer(base_path, std::string("nspawn_") + utils::current_datetime() + "_" + rng.generate(26));
    hcs_create_layer(driver_info, layer, parent_layer_name, acsendant_descriptors);
    auto deferred_destroy_layer = sl::support::defer([&driver_info, &layer]() STATICLIB_NOEXCEPT {
        hcs_destroy_layer(driver_info, layer);
    });

    // activate layer
    hcs_activate_layer(driver_info, layer);
    auto deferred_deactivate_layer = sl::support::defer([&driver_info, &layer]() STATICLIB_NOEXCEPT {
        hcs_deactivate_layer(driver_info, layer);
    });

    // prepare layer
    hcs_prepare_layer(driver_info, layer, acsendant_descriptors);
    auto deferred_unprepare_layer = sl::support::defer([&driver_info, &layer]() STATICLIB_NOEXCEPT {
        hcs_unprepare_layer(driver_info, layer);
    });
    std::string volume_path = hcs_get_layer_mount_path(driver_info, layer);

    // create and start container
    auto cont_conf = container_config(config, base_path, volume_path, layer.clone(), acsendant_layers, rng.generate(8));
    std::cout << "Container config: " << cont_conf.to_json().dumps() << std::endl;
    HANDLE compute_system = hcs_create_compute_system(cont_conf, layer);

    // register callback and wait for container to start
    callback_latch cs_latch;
    hcs_register_compute_system_callback(compute_system, layer, cs_latch);
    hcs_start_compute_system(compute_system, layer, cs_latch);
    auto deferred_terminate_cs = sl::support::defer([&compute_system, &layer, &cs_latch]() STATICLIB_NOEXCEPT {
        hcs_terminate_compute_system(compute_system, layer, cs_latch);
    });

    // list existing containers
    hcs_enumerate_compute_systems();

    // create process and wait for it to exit
    HANDLE process = hcs_create_process(compute_system, config);
    hcs_register_process_callback(process, layer, cs_latch);
}

} // namespace
 
char* hostcompute_nspawn(const char* config_json, int config_json_len) /* noexcept */ {
    if (nullptr == config_json) return sl::utils::alloc_copy(TRACEMSG("Null 'config_json' parameter specified"));
    if (!sl::support::is_uint32_positive(config_json_len)) return sl::utils::alloc_copy(TRACEMSG(
            "Invalid 'config_json_len' parameter specified: [" + sl::support::to_string(config_json_len) + "]"));
    try {
        auto src = sl::io::array_source(config_json, config_json_len);
        auto loaded = sl::json::load(src);
        auto config = nspawn::nspawn_config(loaded);
        nspawn::spawn_and_wait(config);
        return nullptr;
    }
    catch (const std::exception& e) {
        return sl::utils::alloc_copy(TRACEMSG(e.what() + "\nException raised"));
    }
}

void hostcompute_nspawn_free(char* err_message) /* noexcept */ {
    std::free(err_message);
}
