/*
Copyright (C) 2018, Accelize

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <iostream>
#include <iomanip>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <json/json.h>
#include <json/version.h>
#include <thread>
#include <chrono>
#include <numeric>
#include <future>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <fstream>
#include <typeinfo>
#include <sys/types.h>
#include <sys/stat.h>
#include <cmath>
#include <algorithm>

#include "accelize/drm/drm_manager.h"
#include "accelize/drm/version.h"
#include "ws_client.h"
#include "log.h"
#include "utils.h"


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "DrmControllerDataConverter.hpp"
#include "HAL/DrmControllerOperations.hpp"
#pragma GCC diagnostic pop

#define NB_MAX_REGISTER 32

#define TRY try {

#define CATCH_AND_THROW \
    } catch( const Exception &e ) { \
        throw; \
    } catch( const std::exception &e ) { \
        Error( e.what() ); \
        throw; \
    }


namespace Accelize {
namespace DRM {


const char* getApiVersion() {
    return DRMLIB_VERSION;
}


class DRM_LOCAL DrmManager::Impl {

protected:

    // Helper typedef
    typedef std::chrono::steady_clock TClock; /// Shortcut type def to steady clock which is monotonic (so unaffected by clock adjustments)

    // Enum
    enum class eLogFileType: uint8_t {NONE=0, BASIC, ROTATING};
    enum class eLicenseType: uint8_t {METERED, NODE_LOCKED, NONE};
    enum class eMailboxOffset: uint8_t {MB_LOCK_DRM=0, MB_CUSTOM_FIELD, MB_USER};

    // Design constants
    const uint32_t SDK_COMPATIBLITY_LIMIT_MAJOR = 3;
    const uint32_t SDK_COMPATIBLITY_LIMIT_MINOR = 1;

    const uint32_t HDK_COMPATIBLITY_LIMIT_MAJOR = 3;
    const uint32_t HDK_COMPATIBLITY_LIMIT_MINOR = 1;

    const std::map<eLicenseType, std::string> LicenseTypeStringMap = {
            {eLicenseType::NONE       , "Idle"},
            {eLicenseType::METERED    , "Floating/Metering"},
            {eLicenseType::NODE_LOCKED, "Node-Locked"}
    };

#ifdef _WIN32
    const char path_sep = '\\';
# else
    const char path_sep = '/';
#endif

    bool mSecurityStop;

    // Composition
    std::unique_ptr<DrmWSClient> mWsClient;
    std::unique_ptr<DrmControllerLibrary::DrmControllerOperations> mDrmController;
    mutable std::recursive_mutex mDrmControllerMutex;
    bool mIsLockedToDrm = false;

    // Logging parameters
    spdlog::level::level_enum sLogConsoleVerbosity = spdlog::level::err;
    std::string sLogConsoleFormat = std::string("[%^%=8l%$] %-6t, %v");

    spdlog::level::level_enum sLogFileVerbosity = spdlog::level::info;
    std::string  sLogFileFormat       = std::string("%Y-%m-%d %H:%M:%S.%e - %18s:%-4# [%=8l] %=6t, %v");
    eLogFileType sLogFileType         = eLogFileType::NONE;
    std::string  sLogFilePath         = fmt::format( "accelize_drmlib_{}.log", getpid() );
    size_t       sLogFileRotatingSize = 100*1024*1024;
    size_t       sLogFileRotatingNum  = 3;

    spdlog::level::level_enum sLogServiceVerbosity = spdlog::level::info;
    std::string  sLogServiceFormat       = std::string("%Y-%m-%d %H:%M:%S.%e - %18s:%-4# [%=8l] %=6t, %v");
    eLogFileType sLogServiceType         = eLogFileType::NONE;
    std::string  sLogServicePath         = fmt::format( "accelize_drmservice_{}.log", getpid() );
    size_t       sLogServiceRotatingSize = 100*1024*1024;
    size_t       sLogServiceRotatingNum  = 3;

    // Function callbacks
    DrmManager::ReadRegisterCallback  f_read_register;
    DrmManager::WriteRegisterCallback f_write_register;
    DrmManager::AsynchErrorCallback   f_asynch_error;

    // Settings files
    std::string mConfFilePath;
    std::string mCredFilePath;

    // Node-Locked parameters
    std::string mNodeLockLicenseDirPath;
    std::string mNodeLockRequestFilePath;
    std::string mNodeLockLicenseFilePath;

    // License related properties
    uint32_t mWSRetryPeriodLong  = 60;    ///< Time in seconds before the next request attempt to the Web Server when the time left before timeout is large
    uint32_t mWSRetryPeriodShort = 2;     ///< Time in seconds before the next request attempt to the Web Server when the time left before timeout is short
    uint32_t mWSRequestTimeout   = 10;    ///< Time in seconds during which retries occur

    eLicenseType mLicenseType = eLicenseType::METERED;
    uint32_t mLicenseCounter;
    uint32_t mLicenseDuration;

    // Design parameters
    int32_t mFrequencyInit;
    int32_t mFrequencyCurr;
    uint32_t mFrequencyDetectionPeriod = 100;  // in milliseconds
    double mFrequencyDetectionThreshold = 2.0;      // Error in percentage

    // Session state
    std::string mSessionID;
    std::string mUDID;
    std::string mBoardType;

    // Web service communication
    Json::Value mHeaderJsonRequest;

    // thread to maintain alive
    std::future<void> mThreadKeepAlive;
    std::mutex mThreadKeepAliveMtx;
    std::condition_variable mThreadKeepAliveCondVar;
    bool mThreadStopRequest{false};

    // Debug parameters
    spdlog::level::level_enum mDebugMessageLevel;

    // User accessible parameters
    const std::map<ParameterKey, std::string> mParameterKeyMap = {
    #   define PARAMETERKEY_ITEM(id) {id, #id},
    #   include "accelize/drm/ParameterKey.def"
    #   undef PARAMETERKEY_ITEM
        {ParameterKeyCount, "ParameterKeyCount"}
    };


    Impl( const std::string& conf_file_path,
          const std::string& cred_file_path )
    {
        // Basic logging setup
        initLog();

        Json::Value conf_json;

        mSecurityStop = false;
        mIsLockedToDrm = false;

        mLicenseCounter = 0;
        mLicenseDuration = 0;

        mConfFilePath = conf_file_path;
        mCredFilePath = cred_file_path;

        mFrequencyInit = 0;
        mFrequencyCurr = 0;

        mDebugMessageLevel = spdlog::level::trace;

        // Parse configuration file
        conf_json = parseJsonFile( conf_file_path );

        try {
            Json::Value param_lib = JVgetOptional( conf_json, "settings", Json::objectValue );
            if ( param_lib != Json::nullValue ) {
                // Console logging
                sLogConsoleVerbosity = static_cast<spdlog::level::level_enum>( JVgetOptional(
                        param_lib, "log_verbosity", Json::intValue, (int)sLogConsoleVerbosity ).asInt());
                sLogConsoleFormat = JVgetOptional(
                        param_lib, "log_format", Json::stringValue, sLogConsoleFormat ).asString();

                // File logging
                sLogFileVerbosity = static_cast<spdlog::level::level_enum>( JVgetOptional(
                        param_lib, "log_file_verbosity", Json::intValue, (int)sLogFileVerbosity ).asInt() );
                sLogFileFormat = JVgetOptional(
                        param_lib, "log_file_format", Json::stringValue, sLogFileFormat ).asString();
                sLogFilePath = JVgetOptional(
                        param_lib, "log_file_path", Json::stringValue, sLogFilePath ).asString();
                sLogFileType = static_cast<eLogFileType>( JVgetOptional(
                        param_lib, "log_file_type", Json::intValue, (int)sLogFileType ).asInt() );
                sLogFileRotatingSize = JVgetOptional( param_lib, "log_file_rotating_size",
                        Json::intValue, (int)sLogFileRotatingSize ).asInt();
                sLogFileRotatingNum = JVgetOptional( param_lib, "log_file_rotating_num",
                        Json::intValue, (int)sLogFileRotatingNum ).asInt();

                // Service File logging
                sLogServiceVerbosity = static_cast<spdlog::level::level_enum>( JVgetOptional(
                        param_lib, "log_service_verbosity", Json::intValue, (int)sLogServiceVerbosity ).asInt() );
                sLogServiceFormat = JVgetOptional(
                        param_lib, "log_service_format", Json::stringValue, sLogServiceFormat ).asString();
                sLogServicePath = JVgetOptional(
                        param_lib, "log_service_path", Json::stringValue, sLogServicePath ).asString();
                sLogServiceType = static_cast<eLogFileType>( JVgetOptional(
                        param_lib, "log_service_type", Json::intValue, (int)sLogServiceType ).asInt() );
                sLogServiceRotatingSize = JVgetOptional( param_lib, "log_service_rotating_size",
                        Json::intValue, (int)sLogServiceRotatingSize ).asInt();
                sLogServiceRotatingNum = JVgetOptional( param_lib, "log_service_rotating_num",
                        Json::intValue, (int)sLogServiceRotatingNum ).asInt();

                // Frequency detection
                mFrequencyDetectionPeriod = JVgetOptional( param_lib, "frequency_detection_period",
                        Json::uintValue, mFrequencyDetectionPeriod).asUInt();
                mFrequencyDetectionThreshold = JVgetOptional( param_lib, "frequency_detection_threshold",
                        Json::uintValue, mFrequencyDetectionThreshold).asDouble();

                // Others
                mWSRetryPeriodLong = JVgetOptional( param_lib, "ws_retry_period_long",
                        Json::uintValue, mWSRetryPeriodLong).asUInt();
                mWSRetryPeriodShort = JVgetOptional( param_lib, "ws_retry_period_short",
                        Json::uintValue, mWSRetryPeriodShort).asUInt();
                mWSRequestTimeout = JVgetOptional( param_lib, "ws_request_timeout",
                        Json::uintValue, mWSRequestTimeout).asUInt();
                if ( mWSRequestTimeout == 0 )
                    Throw( DRM_BadArg, "ws_request_timeout must not be 0");
            }
            if ( mWSRetryPeriodLong <= mWSRetryPeriodShort )
                Throw( DRM_BadArg, "ws_retry_period_long ({}) must be greater than ws_retry_period_short ({})",
                        mWSRetryPeriodLong, mWSRetryPeriodShort );

            // Customize logging configuration
            updateLog();

            // Design configuration
            Json::Value conf_design = JVgetOptional( conf_json, "design", Json::objectValue );
            if ( !conf_design.empty() ) {
                mUDID = JVgetOptional( conf_design, "udid", Json::stringValue, "" ).asString();
                mBoardType = JVgetOptional( conf_design, "boardType", Json::stringValue, "" ).asString();
            }

            // Licensing configuration
            Json::Value conf_licensing = JVgetRequired( conf_json, "licensing", Json::objectValue );
            // Get licensing mode
            bool is_nodelocked = JVgetOptional( conf_licensing, "nodelocked", Json::booleanValue, false ).asBool();
            if ( is_nodelocked ) {
                // If this is a node-locked license, get the license path
                mNodeLockLicenseDirPath = JVgetRequired( conf_licensing, "license_dir", Json::stringValue ).asString();
                mLicenseType = eLicenseType::NODE_LOCKED;
                Debug( "Configuration file specifies a Node-locked license" );
            } else {
                Debug( "Configuration file specifies a floating/metered license" );
                // Get DRM frequency
                Json::Value conf_drm = JVgetRequired( conf_json, "drm", Json::objectValue );
                mFrequencyInit = JVgetRequired( conf_drm, "frequency_mhz", Json::intValue ).asUInt();
                mFrequencyCurr = mFrequencyInit;
            }

        } catch( Exception &e ) {
            if ( e.getErrCode() != DRM_BadFormat )
                throw;
            Throw( DRM_BadFormat, "Error in configuration file '{}: {}", conf_file_path, e.what() );
        }
    }

    void initLog() {
        try {
            std::vector<spdlog::sink_ptr> sinks;

            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            console_sink->set_level( sLogConsoleVerbosity );
            console_sink->set_pattern( sLogConsoleFormat );
            sinks.push_back( console_sink );

            sLogger = std::make_shared<spdlog::logger>( "drmlib_logger", sinks.begin(), sinks.end() );
            sLogger->set_level( sLogConsoleVerbosity );
            spdlog::set_default_logger( sLogger );
        }
        catch( const spdlog::spdlog_ex& ex ) {
            std::cout << "Failed to initialize logging: " << ex.what() << std::endl;
        }
    }

    void createFileLog( const std::string file_path, const eLogFileType type,
            const spdlog::level::level_enum level, const std::string format,
            const size_t rotating_size, const size_t rotating_num ) {
        spdlog::sink_ptr log_sink;
        std::string version_list = fmt::format( "Installed versions:\n\t-drmlib: {}\n\t-libcurl: {}\n\t-jsoncpp: {}\n\t-spdlog: {}.{}.{}",
                DRMLIB_VERSION, curl_version(), JSONCPP_VERSION_STRING,
                SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR, SPDLOG_VER_PATCH );

        if ( type == eLogFileType::NONE )
            log_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        else {
            // Check if parent directory exists
            std::string parentDir = getDirName( file_path );
            if ( !makeDirs( parentDir ) ) {
                Throw( DRM_ExternFail, "Failed to create log file {}", file_path );
            }
            if ( type == eLogFileType::BASIC )
                log_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                        file_path, true);
            else // type == eLogFileType::ROTATING
                log_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                        file_path, rotating_size, rotating_num);
        }
        log_sink->set_pattern( format );
        log_sink->set_level( spdlog::level::info );
        log_sink->log( spdlog::details::log_msg( "", spdlog::level::info, version_list ) );
        log_sink->set_level( level );
        sLogger->sinks().push_back( log_sink );
        if ( level < sLogger->level() )
            sLogger->set_level( level );
        Debug( "Created log file '{}' of type {}, with verbosity {}", file_path, (int)type, (int)level );
    }

    void updateLog() {
        try {
            auto console_sink = sLogger->sinks()[0];
            console_sink->set_level( sLogConsoleVerbosity );
            console_sink->set_pattern( sLogConsoleFormat );
            if ( sLogConsoleVerbosity < sLogger->level() )
                sLogger->set_level( sLogConsoleVerbosity );

            // File logging
            createFileLog( sLogFilePath, sLogFileType, sLogFileVerbosity, sLogFileFormat,
                    sLogFileRotatingSize, sLogFileRotatingNum );

            // Service logging
            if ( sLogServiceType != eLogFileType::NONE ) {
                createFileLog( sLogServicePath, sLogServiceType, sLogServiceVerbosity,
                               sLogServiceFormat, sLogServiceRotatingSize, sLogServiceRotatingNum );
            }
        }
        catch( const spdlog::spdlog_ex& ex ) {
            std::cout << "Failed to update logging settings: " << ex.what() << std::endl;
        }
    }

    void uninitLog() {
        if ( sLogger )
            sLogger->flush();
    }

    uint32_t getMailboxSize() const {
        uint32_t roSize, rwSize;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeMailBoxFilePageRegister() );
        checkDRMCtlrRet( getDrmController().readMailboxFileSizeRegister( roSize, rwSize ) );
        Debug2( "Read Mailbox size: {}", rwSize );
        return rwSize;
    }

    uint32_t readMailbox( const eMailboxOffset offset ) const {
        auto index = (uint32_t)offset;
        uint32_t roSize, rwSize;
        std::vector<uint32_t> roData, rwData;

        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeMailBoxFilePageRegister() );
        checkDRMCtlrRet( getDrmController().readMailboxFileRegister( roSize, rwSize, roData, rwData) );

        if ( index >= rwData.size() )
            Unreachable( "Index ", index, " overflows the Mailbox memory; max index is ",
                    rwData.size()-1 ); //LCOV_EXCL_LINE

        Debug( "Read '{}' in Mailbox at index {}", rwData[index], index );
        return rwData[index];
    }

    std::vector<uint32_t> readMailbox( const eMailboxOffset offset, const uint32_t& nb_elements ) const {
        auto index = (uint32_t)offset;
        uint32_t roSize, rwSize;
        std::vector<uint32_t> roData, rwData;

        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeMailBoxFilePageRegister() );
        checkDRMCtlrRet( getDrmController().readMailboxFileRegister( roSize, rwSize, roData, rwData) );

        if ( (uint32_t)index >= rwData.size() )
            Unreachable( "Index {} overflows the Mailbox memory; max index is {}",
                    index, rwData.size()-1 ); //LCOV_EXCL_LINE
        if ( index + nb_elements > rwData.size() )
            Throw( DRM_BadArg, "Trying to read out of Mailbox memory space; size is {}", rwData.size() );

        auto first = rwData.cbegin() + index;
        auto last = rwData.cbegin() + index + nb_elements;
        std::vector<uint32_t> value_vec( first, last );
        Debug( "Read {} elements in Mailbox from index {}", value_vec.size(), index);
        return value_vec;
    }

    void writeMailbox( const eMailboxOffset offset, const uint32_t& value ) const {
        auto index = (uint32_t)offset;
        uint32_t roSize, rwSize;
        std::vector<uint32_t> roData, rwData;

        std::lock_guard<std::recursive_mutex> lockk( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeMailBoxFilePageRegister() );
        checkDRMCtlrRet( getDrmController().readMailboxFileRegister( roSize, rwSize, roData, rwData) );

        if ( index >= rwData.size() )
            Unreachable( "Index ", index, " overflows the Mailbox memory: max index is ", rwData.size()-1 ); //LCOV_EXCL_LINE
        rwData[index] = value;
        checkDRMCtlrRet( getDrmController().writeMailboxFileRegister( rwData, rwSize ) );
        Debug( "Wrote '{}' in Mailbox at index {}", value, index );
    }

    void writeMailbox( const eMailboxOffset offset, const std::vector<uint32_t> &value_vec ) const {
        auto index = (uint32_t)offset;
        uint32_t roSize, rwSize;
        std::vector<uint32_t> roData, rwData;

        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeMailBoxFilePageRegister() );
        checkDRMCtlrRet( getDrmController().readMailboxFileRegister( roSize, rwSize, roData, rwData) );
        if ( index >= rwData.size() )
            Unreachable( "Index {} overflows the Mailbox memory: max index is {}",
                    index, rwData.size()-1 ); //LCOV_EXCL_LINE
        if ( index + value_vec.size() > rwData.size() )
            Throw( DRM_BadArg, "Trying to write out of Mailbox memory space: {}", rwData.size() );
        std::copy( std::begin( value_vec ), std::end( value_vec ), std::begin( rwData ) + index );
        checkDRMCtlrRet( getDrmController().writeMailboxFileRegister( rwData, rwSize ) );
        Debug( "Wrote {} elements in Mailbox from index {}", value_vec.size(), index );
    }

    DrmControllerLibrary::DrmControllerOperations& getDrmController() const {
        if ( mDrmController )
            return *mDrmController;
        Unreachable( "No DRM Controller available" ); //LCOV_EXCL_LINE
    }

    DrmWSClient& getDrmWSClient() const {
        if ( mWsClient )
            return *mWsClient;
        Unreachable( "No Web Service has been defined" ); //LCOV_EXCL_LINE
    }

    static uint32_t getDrmRegisterOffset( const std::string& regName ) {
        if ( regName == "DrmPageRegister" )
            return 0;
        if ( regName.substr( 0, 15 ) == "DrmRegisterLine" )
            return (uint32_t)std::stoul( regName.substr( 15 ) ) * 4 + 4;
        Unreachable( "Unsupported regName argument: ", regName ); //LCOV_EXCL_LINE
    }

    unsigned int readDrmRegister( const std::string& regName, unsigned int& value ) const {
        int ret = 0;
        ret = f_read_register( getDrmRegisterOffset( regName ), &value );
        if ( ret != 0 ) {
            Error( "Error in read register callback, errcode = {}", ret );
            return (uint32_t)(-1);
        }
        Debug2( "Read DRM register @{} = 0x{:08x}", regName, value );
        return 0;
    }

    unsigned int writeDrmRegister( const std::string& regName, unsigned int value ) const {
        int ret = 0;
        ret = f_write_register( getDrmRegisterOffset( regName ), value );
        if ( ret ) {
            Error( "Error in write register callback, errcode = {}", ret );
            return (uint32_t)(-1);
        }
        Debug2( "Write DRM register @{} = {:08x}", regName, value );
        return 0;
    }

    void checkDRMCtlrRet( const unsigned int& errcode ) const {
        if ( errcode )
            Unreachable( "Error in DRM Controller library call: ", errcode ); //LCOV_EXCL_LINE
    }

    void lockDrmToInstance() {
        return;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        uint32_t isLocked = readMailbox( eMailboxOffset::MB_LOCK_DRM );
        if ( isLocked )
            Throw( DRM_BadUsage, "Another instance of the DRM Manager is currently owning the HW" );
        writeMailbox( eMailboxOffset::MB_LOCK_DRM, 1 );
        mIsLockedToDrm = true;
        Debug( "DRM Controller is now locked to this object instance" );
    }

    void unlockDrmToInstance() {
        return;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        if ( !mIsLockedToDrm )
            return;
        uint32_t isLocked = readMailbox( eMailboxOffset::MB_LOCK_DRM );
        if ( isLocked ) {
            writeMailbox( eMailboxOffset::MB_LOCK_DRM, 0 );
            Debug( "DRM Controller is now unlocked to this object instance" );
        }
    }

    // Check compatibility of the DRM Version with Algodone version
    void checkHdkCompatibility() const {
        uint32_t drmVersionNum;
        std::string drmVersionDot;
        std::string drmVersion = getDrmCtrlVersion();

        drmVersionNum = DrmControllerLibrary::DrmControllerDataConverter::hexStringToBinary( drmVersion )[0];
        drmVersionDot = DrmControllerLibrary::DrmControllerDataConverter::binaryToVersionString( drmVersionNum );

        auto drmMajor = ( drmVersionNum >> 16 ) & 0xFF;
        auto drmMinor = ( drmVersionNum >> 8  ) & 0xFF;

        if ( drmMajor < HDK_COMPATIBLITY_LIMIT_MAJOR ) {
            Throw( DRM_CtlrError,
                    "This DRM Lib {} is not compatible with the DRM HDK version {}: To be compatible HDK version shall be > or equal to {}.{}.0",
                    DRMLIB_VERSION, drmVersionDot, HDK_COMPATIBLITY_LIMIT_MAJOR, HDK_COMPATIBLITY_LIMIT_MINOR );
        }
        if ( drmMinor < HDK_COMPATIBLITY_LIMIT_MINOR ) {
            Throw( DRM_CtlrError,
                    "This DRM Library version {} is not compatible with the DRM HDK version {}: To be compatible HDK version shall be > or equal to {}.{}.0",
                    DRMLIB_VERSION, drmVersionDot, HDK_COMPATIBLITY_LIMIT_MAJOR, HDK_COMPATIBLITY_LIMIT_MINOR );
        }
        Debug( "DRM HDK Version: {}", drmVersionDot );
    }

    void initDrmInterface() {

        if ( mDrmController )
            return;

        // create instance
        try {
            mDrmController.reset(
                    new DrmControllerLibrary::DrmControllerOperations(
                            std::bind( &DrmManager::Impl::readDrmRegister,
                                       this,
                                       std::placeholders::_1,
                                       std::placeholders::_2 ),
                            std::bind( &DrmManager::Impl::writeDrmRegister,
                                       this,
                                       std::placeholders::_1,
                                       std::placeholders::_2 )
                    ));
        } catch( const std::exception& e ) {
            std::string err_msg(e.what());
            if ( err_msg.find( "Unable to select a register strategy that is compatible with the DRM Controller" )
                    != std::string::npos )
                Throw( DRM_CtlrError, "Unable to find DRM Controller registers. Please check:\n"
                                      "\t- The DRM offset in your read/write callback implementation,\n"
                                      "\t- The compatibility between the SDK and DRM HDK in use");
            Throw( DRM_CtlrError, "Failed to initialize DRM Controller: {}", e.what() );
        }
        Debug( "DRM Controller SDK is initialized" );

        // Check compatibility of the DRM Version with Algodone version
        checkHdkCompatibility();

        // Try to lock the DRM controller to this instance, return an error is already locked.
        lockDrmToInstance();

        // Save header information
        mHeaderJsonRequest = getMeteringHeader();

        // If node-locked license is requested, create license request file
        if ( mLicenseType == eLicenseType::NODE_LOCKED ) {

            // Check license directory exists
            if ( !isDir( mNodeLockLicenseDirPath ) )
                Throw( DRM_BadArg,
                        "License directory path '{}' specified in configuration file '{}' is not existing on file system",
                        mNodeLockLicenseDirPath, mConfFilePath );

            // If a floating/metering session is still running, try to close it gracefully.
            if ( isDrmCtrlInMetering() && isSessionRunning() ) {
                Debug( "A floating/metering session is still pending: trying to close it gracefully before switching to nodelocked license." );
                mHeaderJsonRequest["mode"] = (uint8_t)eLicenseType::METERED;
                try {
                    mWsClient.reset( new DrmWSClient( mConfFilePath, mCredFilePath ) );
                    stopSession();
                } catch( const Exception& e ) {
                    Debug( "Failed to stop gracefully the pending session because: {}", e.what() );
                }
                mHeaderJsonRequest["mode"] = (uint8_t)eLicenseType::NODE_LOCKED;
            }

            // Create license request file
            createNodelockedLicenseRequestFile();
        } else {
            mWsClient.reset( new DrmWSClient( mConfFilePath, mCredFilePath ) );
        }
    }

    void checkSessionIDFromWS( const Json::Value license_json ) {
        std::string ws_sessionID = license_json["metering"]["sessionId"].asString();
        if ( !mSessionID.empty() && ( mSessionID != ws_sessionID ) ) {
            Unreachable( "Session ID mismatch: received '", ws_sessionID, "' from WS but expect '",
                    mSessionID, "'"); //LCOV_EXCL_LINE
        }
    }

    void checkSessionIDFromDRM( const Json::Value license_json ) {
        std::string ws_sessionID = license_json["sessionId"].asString();
        if ( !mSessionID.empty() && ( mSessionID != ws_sessionID ) ) {
            Unreachable( "Session ID mismatch: DRM gives '", ws_sessionID, "' but expect '",
                    mSessionID, "'"); //LCOV_EXCL_LINE
        }
    }

    void getNumActivator( uint32_t& value ) const {
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeRegistersPageRegister() );
        checkDRMCtlrRet( getDrmController().readNumberOfDetectedIpsStatusRegister( value ) );
    }

    uint64_t getTimerCounterValue() const {
        uint32_t licenseTimerCounterMsb(0), licenseTimerCounterLsb(0);
        uint64_t licenseTimerCounter(0);
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().sampleLicenseTimerCounter( licenseTimerCounterMsb,
                licenseTimerCounterLsb ) );
        licenseTimerCounter = licenseTimerCounterMsb;
        licenseTimerCounter <<= 32;
        licenseTimerCounter |= licenseTimerCounterLsb;
        return licenseTimerCounter;
    }

    std::string getDrmPage( uint32_t page_index ) const {
        uint32_t value;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        writeDrmRegister( "DrmPageRegister", page_index );
        std::string str = fmt::format( "DRM Page {}  registry:\n", page_index );
        for( uint32_t r=0; r < NB_MAX_REGISTER; r++ ) {
            f_read_register( r*4, &value );
            str += fmt::format( "\tRegister @0x{:02X}: 0x{:08X} ({:d})\n", r*4, value, value );
        }
        return str;
    }

    std::string getDrmReport() const {
        std::stringstream ss;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        getDrmController().printHwReport( ss );
        return ss.str();
    }

    uint64_t getMeteringData() const {
        uint32_t numberOfDetectedIps;
        std::string saasChallenge;
        std::vector<std::string> meteringFile;
        uint64_t meteringData = 0;

        Debug2( "Get metering data from session on DRM controller" );

        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        if ( ( mLicenseType == eLicenseType::NODE_LOCKED ) || isLicenseActive() ) {
            checkDRMCtlrRet( getDrmController().asynchronousExtractMeteringFile(
                    numberOfDetectedIps, saasChallenge, meteringFile ) );
            std::string meteringDataStr = meteringFile[2].substr( 16, 16 );
            errno = 0;
            meteringData = strtoull( meteringDataStr.c_str(), nullptr, 16 );
            if ( errno )
                Throw( DRM_CtlrError, "Could not convert string '{}' to unsigned long long.",
                        meteringDataStr );
            return meteringData;
        } else {
            return 0;
        }
    }

    // Get DRM HDK version
    std::string getDrmCtrlVersion() const {
        std::string drmVersion;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().extractDrmVersion( drmVersion ) );
        return drmVersion;
    }

    // Get common info
    void getDesignInfo( std::string &drmVersion,
                        std::string &dna,
                        std::vector<std::string> &vlnvFile,
                        std::string &mailboxReadOnly ) {
        uint32_t nbOfDetectedIps;
        uint32_t readOnlyMailboxSize, readWriteMailboxSize;
        std::vector<uint32_t> readOnlyMailboxData, readWriteMailboxData;

        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().extractDrmVersion( drmVersion ) );
        checkDRMCtlrRet( getDrmController().extractDna( dna ) );
        checkDRMCtlrRet( getDrmController().extractVlnvFile( nbOfDetectedIps, vlnvFile ) );
        checkDRMCtlrRet( getDrmController().readMailboxFileRegister( readOnlyMailboxSize, readWriteMailboxSize,
                                                                     readOnlyMailboxData, readWriteMailboxData ) );
        Debug( "Mailbox sizes: read-only={}, read-write={}", readOnlyMailboxSize, readWriteMailboxSize );
        readOnlyMailboxData.push_back( 0 );
        if ( readOnlyMailboxSize ) {
            mailboxReadOnly = std::string( (char*)readOnlyMailboxData.data() );
        }
        else
            mailboxReadOnly = std::string("");
    }

    Json::Value getMeteringHeader() {
        Json::Value json_output;
        std::string drmVersion;
        std::string dna;
        std::vector<std::string> vlnvFile;
        std::string mailboxReadOnly;

        // Fulfill application section
        if ( !mUDID.empty() )
            json_output["udid"] = mUDID;
        if ( !mBoardType.empty() )
            json_output["boardType"] = mBoardType;
        json_output["mode"] = (uint8_t) mLicenseType;
        if ( mLicenseType != eLicenseType::NODE_LOCKED )
            json_output["drm_frequency_init"] = mFrequencyInit;

        // Get information from DRM Controller
        getDesignInfo( drmVersion, dna, vlnvFile, mailboxReadOnly );

        // Fulfill DRM section
        json_output["drmlibVersion"] = DRMLIB_VERSION;
        json_output["lgdnVersion"] = drmVersion;
        json_output["dna"] = dna;
        for ( uint32_t i = 0; i < vlnvFile.size(); i++ ) {
            std::string i_str = std::to_string(i);
            json_output["vlnvFile"][i_str]["vendor"] = std::string("x") + vlnvFile[i].substr(0, 4);
            json_output["vlnvFile"][i_str]["library"] = std::string("x") + vlnvFile[i].substr(4, 4);
            json_output["vlnvFile"][i_str]["name"] = std::string("x") + vlnvFile[i].substr(8, 4);
            json_output["vlnvFile"][i_str]["version"] = std::string("x") + vlnvFile[i].substr(12, 4);
        }

        if ( !mailboxReadOnly.empty() ) {
            try {
                json_output["product"] = parseJsonString( mailboxReadOnly );
            } catch( const Exception &e ) {
                if ( e.getErrCode() == DRM_BadFormat )
                    Throw( DRM_BadFormat, "Failed to parse Read-Only Mailbox in DRM Controller: {}", e.what() );
                throw;
            }
        } else {
            Debug( "Could not find product ID information in DRM Controller Mailbox" );
        }
        return json_output;
    }

    Json::Value getMeteringStart() {
        Json::Value json_request( mHeaderJsonRequest );
        uint32_t numberOfDetectedIps;
        std::string saasChallenge;
        std::vector<std::string> meteringFile;

        Debug( "Build web request to create new session" );
        mLicenseCounter = 0;
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().initialization( numberOfDetectedIps, saasChallenge, meteringFile ) );
        json_request["saasChallenge"] = saasChallenge;
        json_request["meteringFile"]  = std::accumulate( meteringFile.begin(), meteringFile.end(), std::string("") );
        json_request["request"] = "open";
        if ( mLicenseType != eLicenseType::NODE_LOCKED )
            json_request["drm_frequency"] = mFrequencyCurr;
        json_request["mode"] = (uint8_t)mLicenseType;

        return json_request;
    }

    Json::Value getMeteringWait() {
        Json::Value json_request( mHeaderJsonRequest );
        uint32_t numberOfDetectedIps;
        std::string saasChallenge;
        std::vector<std::string> meteringFile;

        Debug( "Build web request to maintain current session" );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().synchronousExtractMeteringFile( numberOfDetectedIps, saasChallenge, meteringFile ) );
        json_request["saasChallenge"] = saasChallenge;
        json_request["sessionId"] = meteringFile[0].substr( 0, 16 );
        checkSessionIDFromDRM( json_request );
        if ( mLicenseType != eLicenseType::NODE_LOCKED )
            json_request["drm_frequency"] = mFrequencyCurr;
        json_request["meteringFile"] = std::accumulate( meteringFile.begin(), meteringFile.end(), std::string("") );
        json_request["request"] = "running";
        return json_request;
    }

    Json::Value getMeteringStop() {
        Json::Value json_request( mHeaderJsonRequest );
        uint32_t numberOfDetectedIps;
        std::string saasChallenge;
        std::vector<std::string> meteringFile;

        Debug( "Build web request to stop current session" );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().endSessionAndExtractMeteringFile( numberOfDetectedIps, saasChallenge, meteringFile ) );
        json_request["saasChallenge"] = saasChallenge;
        json_request["sessionId"] = meteringFile[0].substr( 0, 16 );
        checkSessionIDFromDRM( json_request );
        if ( mLicenseType != eLicenseType::NODE_LOCKED )
            json_request["drm_frequency"] = mFrequencyCurr;
        json_request["meteringFile"]  = std::accumulate( meteringFile.begin(), meteringFile.end(), std::string("") );
        json_request["request"] = "close";
        return json_request;
    }

    bool isSessionRunning()const  {
        bool sessionRunning( false );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeRegistersPageRegister() );
        checkDRMCtlrRet( getDrmController().readSessionRunningStatusRegister( sessionRunning ) );
        Debug( "DRM session running state: {}", sessionRunning );
        return sessionRunning;
    }

    bool isDrmCtrlInNodelock()const  {
        bool isNodelocked( false );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeRegistersPageRegister() );
        checkDRMCtlrRet( getDrmController().readLicenseNodeLockStatusRegister( isNodelocked ) );
        Debug( "DRM Controller node-locked status: {}", isNodelocked );
        return isNodelocked;
    }

    bool isDrmCtrlInMetering()const  {
        bool isMetering( false );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeRegistersPageRegister() );
        checkDRMCtlrRet( getDrmController().readLicenseMeteringStatusRegister( isMetering ) );
        Debug( "DRM Controller metering status: {}", isMetering );
        return isMetering;
    }

    bool isReadyForNewLicense() const {
        bool ret( false );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeRegistersPageRegister() );
        checkDRMCtlrRet( getDrmController().readLicenseTimerInitLoadedStatusRegister( ret ) );
        Debug( "DRM readiness to receive a new license: {}", !ret );
        return !ret;
    }

    bool isLicenseActive() const {
        bool isLicenseEmpty( false );
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );
        checkDRMCtlrRet( getDrmController().writeRegistersPageRegister() );
        checkDRMCtlrRet( getDrmController().readLicenseTimerCountEmptyStatusRegister( isLicenseEmpty ) );
        return !isLicenseEmpty;
    }

    Json::Value getLicense( const Json::Value& request_json, const uint32_t& timeout,
            const uint32_t& short_retry_period = 0, const uint32_t& long_retry_period = 0 ) {
        TClock::time_point deadline = TClock::now() + std::chrono::seconds( timeout );
        return getLicense( request_json, deadline, short_retry_period, long_retry_period );
    }

    Json::Value getLicense( const Json::Value& request_json, const TClock::time_point& deadline,
            const uint32_t& short_retry_period = 0, const uint32_t& long_retry_period = 0 ) {

        TClock::duration long_duration = std::chrono::seconds( long_retry_period );
        TClock::duration short_duration = std::chrono::seconds( short_retry_period );

        // Get valid OAUth2 token
        uint32_t attempt = 0;
        TClock::duration wait_duration;
        while ( 1 ) {
            try {
                getDrmWSClient().requestOAuth2token( deadline );
                break;
            } catch ( const Exception& e ) {
                if ( e.getErrCode() != DRM_WSMayRetry ) {
                    throw;
                }
                // It is retryable
                attempt ++;
                if ( TClock::now() > deadline ) {
                    // Reached timeout
                    Throw( DRM_WSError, "Timeout on Authentication request after {} attempts", attempt );
                }
                if ( short_retry_period == 0 ) {
                    // No retry
                    throw;
                }
                // Perform retry
                if ( long_retry_period == 0 ) {
                     wait_duration = short_duration;
                } else {
                    if ( ( deadline - TClock::now() ) < long_duration )
                        wait_duration = short_duration;
                    else
                        wait_duration = long_duration;
                }
                Warning( "Attempt #{} to obtain a new OAuth2 token failed with message: {}. New attempt planned in {} seconds",
                        attempt, e.what(), wait_duration.count()/1000000000 );
                // Wait a bit before retrying
                sleepOrExit( wait_duration );
            }
        }

        // Get new license
        attempt = 0;
        while ( 1 ) {
            try {
                return getDrmWSClient().requestLicense( request_json, deadline );
            } catch ( const Exception& e ) {
                if ( e.getErrCode() != DRM_WSMayRetry ) {
                    throw;
                }
                // It is retryable
                attempt ++;
                if ( TClock::now() > deadline ) {
                    // Reached timeout
                    Throw( DRM_WSError, "Timeout on License request after {} attempts", attempt );
                }
                if ( short_retry_period == 0 ) {
                    // No retry
                    throw;
                }
                // Perform retry
                if ( long_retry_period == 0 ) {
                     wait_duration = short_duration;
                } else {
                    if ( ( deadline - TClock::now() ) < long_duration )
                        wait_duration = short_duration;
                    else
                        wait_duration = long_duration;
                }
                Warning( "Attempt #{} to obtain a new License failed with message: {}. New attempt planned in {} seconds",
                        attempt, e.what(), wait_duration.count()/1000000000 );
                // Wait a bit before retrying
                sleepOrExit( wait_duration );
            }
        }
    }

    void setLicense( const Json::Value& license_json ) {
        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );

        Debug( "Installing next license on DRM controller" );

        std::string dna = mHeaderJsonRequest["dna"].asString();
        std::string licenseKey, licenseTimer;

        try {
            Json::Value metering_node = JVgetRequired( license_json, "metering", Json::objectValue );
            Json::Value license_node = JVgetRequired( license_json, "license", Json::objectValue );
            Json::Value dna_node = JVgetRequired( license_node, dna.c_str(), Json::objectValue );

            /// Get session ID received from web service
            if ( mSessionID.empty() ) {
                /// Save new Session ID
                mSessionID = JVgetRequired( metering_node, "sessionId", Json::stringValue ).asString();
                Debug( "Saving session ID: {}", mSessionID );
            } else {
                /// Verify Session ID
                checkSessionIDFromWS( license_json );
            }

            // Extract license and license timer from web service response
            licenseKey = JVgetRequired( dna_node, "key", Json::stringValue ).asString();
            if ( mLicenseType != eLicenseType::NODE_LOCKED ) {
                licenseTimer = JVgetRequired( dna_node, "licenseTimer", Json::stringValue ).asString();
                mLicenseDuration = JVgetRequired( metering_node, "timeoutSecond", Json::uintValue ).asUInt();
                if ( mLicenseDuration == 0 ) {
                    Warning( "'timeoutSecond' field sent by License WS must not be 0" );
                }
            }

        } catch( Exception &e ) {
            if ( e.getErrCode() != DRM_BadFormat )
                throw;
            Throw( DRM_WSRespError, "Malformed response from License Web Service: {}", e.what() );
        }

        // Activate
        bool activationDone = false;
        uint8_t activationErrorCode;
        checkDRMCtlrRet( getDrmController().activate( licenseKey, activationDone, activationErrorCode ) );
        if ( activationErrorCode ) {
            Throw( DRM_CtlrError, "Failed to activate license on DRM controller, activationErr: 0x{:x}",
                  activationErrorCode );
        }

        // Load license timer
        if ( mLicenseType != eLicenseType::NODE_LOCKED ) {
            bool licenseTimerEnabled = false;
            checkDRMCtlrRet(
                    getDrmController().loadLicenseTimerInit( licenseTimer, licenseTimerEnabled ) );
            if ( !licenseTimerEnabled ) {
                Throw( DRM_CtlrError,
                      "Failed to load license timer on DRM controller, licenseTimerEnabled: 0x{:x}",
                      licenseTimerEnabled );
            }

            Debug( "Set license #{} of session ID {} for a duration of {} seconds",
                    ++mLicenseCounter, mSessionID, mLicenseDuration );
        }

        // Check DRM Controller has switched to the right license mode
        bool is_nodelocked = isDrmCtrlInNodelock();
        bool is_metered = isDrmCtrlInMetering();
        if ( is_nodelocked && is_metered )
            Unreachable( "DRM Controller cannot be in both Node-Locked and Metering/Floating license modes" ); //LCOV_EXCL_LINE
        if ( mLicenseType != eLicenseType::NODE_LOCKED ) {
            if ( !is_metered )
                Throw( DRM_CtlrError, "DRM Controller failed to switch to Metering license mode" );
            else
                Debug( "DRM Controller is in Metering license mode" );
        } else { // mLicenseType == eLicenseType::NODE_LOCKED
            if ( !is_nodelocked )
                Throw( DRM_CtlrError, "DRM Controller failed to switch to Node-Locked license mode" );
            else
                Debug( "DRM Controller is in Node-Locked license mode" );
        }
    }

    std::string getDesignHash() {
        std::string drmVersion, dna, mailboxReadOnly;
        std::vector<std::string> vlnvFile;
        std::hash<std::string> hasher;

        getDesignInfo( drmVersion, dna, vlnvFile, mailboxReadOnly );
        std::string design = dna + drmVersion;
        for( const std::string& vlnv: vlnvFile )
            design += vlnv;
        std::string hash = fmt::format( "{:016X}", hasher( design ) );
        Debug( "Hash for HW design is {}", hash );
        return hash;
    }

    void createNodelockedLicenseRequestFile() {
        // Create hash name based on design info
        std::string designHash = getDesignHash();
        mNodeLockRequestFilePath = mNodeLockLicenseDirPath + path_sep + designHash + ".req";
        mNodeLockLicenseFilePath = mNodeLockLicenseDirPath + path_sep + designHash + ".lic";
        Debug( "Created hash name based on design info: {}", designHash );
        // Check if license request file already exists
        if ( isFile( mNodeLockRequestFilePath ) ) {
            Debug( "A license request file is already existing in license directory: {}", mNodeLockLicenseDirPath );
            return;
        }
        // Build request for node-locked license
        Json::Value request_json = getMeteringStart();
        Debug( "License request JSON:\n{}", request_json.toStyledString() );

        // Save license request to file
        saveJsonToFile( mNodeLockRequestFilePath, request_json );
        Debug( "License request file saved on: {}", mNodeLockRequestFilePath );
    }

    void installNodelockedLicense() {
        Json::Value license_json;
            std::ifstream ifs;

        Debug ( "Looking for local node-locked license file: {}", mNodeLockLicenseFilePath );

        try {
            // Try to load a local license file
            license_json = parseJsonFile( mNodeLockLicenseFilePath );
            Debug( "Parsed Node-locked License file: {}", license_json .toStyledString() );

        } catch( const Exception& e ) {
            /// No license has been found locally, request one to License WS:
            /// - Clear Session IS
            Debug( "Clearing session ID: {}", mSessionID );
            mSessionID = std::string("");
            /// - Create WS access
            mWsClient.reset( new DrmWSClient( mConfFilePath, mCredFilePath ) );
            /// - Read request file
            try {
                Json::Value request_json = parseJsonFile( mNodeLockRequestFilePath );
                Debug( "Parsed Node-locked License Request file: {}", request_json .toStyledString() );
                /// - Send request to web service and receive the new license
                TClock::time_point deadline =
                        TClock::now() + std::chrono::seconds( mWSRequestTimeout );
                license_json = getLicense( request_json, deadline, mWSRetryPeriodShort );
                /// - Save the license to file
                saveJsonToFile( mNodeLockLicenseFilePath, license_json );
                Debug( "Requested and saved new node-locked license file: {}", mNodeLockLicenseFilePath );
            } catch( const Exception& e ) {
                Throw( e.getErrCode(), "Failed to request license file: {}", e.what() );
            }
        }
        /// Install the license
        setLicense( license_json );
        Info( "Installed node-locked license successfully" );
    }

    void detectDrmFrequency() {
        TClock::time_point timeStart, timeEnd;
        uint64_t counterStart, counterEnd;
        TClock::duration wait_duration = std::chrono::milliseconds( mFrequencyDetectionPeriod );
        int max_attempts = 3;

        std::lock_guard<std::recursive_mutex> lock( mDrmControllerMutex );

        Debug( "Detecting DRM frequency for {} ms", mFrequencyDetectionPeriod );

        while ( max_attempts > 0 ) {

            counterStart = getTimerCounterValue();
            // Wait until counter starts decrementing
            while (1) {
                if (getTimerCounterValue() < counterStart) {
                    counterStart = getTimerCounterValue();
                    timeStart = TClock::now();
                    break;
                }
            }

            /// Wait a fixed period of time
            sleepOrExit(wait_duration);

            counterEnd = getTimerCounterValue();
            timeEnd = TClock::now();

            if (counterEnd == 0)
                Unreachable(
                        "Frequency auto-detection failed: license timeout counter is 0"); //LCOV_EXCL_LINE
            if (counterEnd > counterStart)
                Debug("License timeout counter has been reset: taking another sample");
            else
                break;
            max_attempts--;
        }
        if ( max_attempts == 0 )
            Unreachable("Failed to estimate DRM frequency after 3 attempts"); //LCOV_EXCL_LINE

        Debug( "Start time = {} / Counter start = {}", timeStart.time_since_epoch().count(), counterStart );
        Debug( "End time = {} / Counter end = {}", timeEnd.time_since_epoch().count(), counterEnd );

        // Compute estimated DRM frequency
        TClock::duration timeSpan = timeEnd - timeStart;
        double seconds = double( timeSpan.count() ) * TClock::period::num / TClock::period::den;
        auto ticks = (uint32_t)(counterStart - counterEnd);
        auto measuredFrequency = (int32_t)(std::ceil((double)ticks / seconds / 1000000));
        Debug( "Duration = {} s   /   ticks = {}   =>   estimated frequency = {} MHz", seconds, ticks, measuredFrequency );

        // Compuate precision error compared to config file
        double precisionError = 100.0 * abs( measuredFrequency - mFrequencyCurr ) / mFrequencyCurr ; // At that point mFrequencyCurr = mFrequencyInit
        if ( precisionError >= mFrequencyDetectionThreshold ) {
            mFrequencyCurr = measuredFrequency;
            Throw( DRM_BadFrequency,
                    "Estimated DRM frequency ({} MHz) differs from the value ({} MHz) defined in the configuration file '{}' by more than {}%: From now on the considered frequency is {} MHz",
                    mFrequencyCurr, mFrequencyInit, mConfFilePath, mFrequencyDetectionThreshold, mFrequencyCurr);
        } else {
            Debug( "Estimated DRM frequency = {} MHz, config frequency = {} MHz: gap = {}%",
                    measuredFrequency, mFrequencyInit, precisionError );
        }
    }

    template< class Clock, class Duration >
    void sleepOrExit(
            const std::chrono::time_point<Clock, Duration> &timeout_time) {
        std::unique_lock<std::mutex> lock( mThreadKeepAliveMtx );
        bool isExitRequested = mThreadKeepAliveCondVar.wait_until( lock, timeout_time,
                [ this ]{ return mThreadStopRequest; } );
        if ( isExitRequested )
            Throw( DRM_Exit, "Exit requested" );
    }

    template< class Rep, class Period >
    void sleepOrExit( const std::chrono::duration<Rep, Period> &rel_time ) {
        std::unique_lock<std::mutex> lock( mThreadKeepAliveMtx );
        bool isExitRequested = mThreadKeepAliveCondVar.wait_for( lock, rel_time,
                [ this ]{ return mThreadStopRequest; } );
        if ( isExitRequested )
            Throw( DRM_Exit, "Exit requested" );
    }

    bool isStopRequested() {
        std::lock_guard<std::mutex> lock( mThreadKeepAliveMtx );
        return mThreadStopRequest;
    }

    uint32_t getCurrentLicenseTimeLeft() {
        uint64_t counterCurr = getTimerCounterValue();
        return (uint32_t)std::ceil( (double)counterCurr / mFrequencyCurr / 1000000 );
    }

    void startLicenseContinuityThread() {

        if ( mThreadKeepAlive.valid() ) {
            Warning( "Thread already started" );
            return;
        }

        Debug( "Starting background thread which maintains licensing" );

        mThreadKeepAlive = std::async( std::launch::async, [ this ]() {
            try {
                /// Detecting DRM controller frequency
                detectDrmFrequency();

                /// Starting license request loop
                while( 1 ) {

                    // Check DRM licensing queue
                    if ( !isReadyForNewLicense() ) {
                        // DRM licensing queue is full, wait until current license expires
                        uint32_t licenseTimeLeft = getCurrentLicenseTimeLeft();
                        TClock::duration wait_duration = std::chrono::seconds( licenseTimeLeft + 1 );
                        Debug( "Sleeping for {} seconds before checking DRM Controller readiness for a new license",
                                licenseTimeLeft );
                        sleepOrExit( wait_duration );

                    } else {
                        if ( isStopRequested() )
                            return;

                        Debug( "Requesting a new license now" );

                        Json::Value request_json = getMeteringWait();
                        Json::Value license_json;

                        /// Retry Web Service request loop
                        TClock::time_point polling_deadline = TClock::now()
                                + std::chrono::seconds( mLicenseDuration );

                        /// Attempt to get the next license
                        license_json = getLicense( request_json, polling_deadline,
                                mWSRetryPeriodShort, mWSRetryPeriodLong );

                        /// New license has been received: now send it to the DRM Controller
                        setLicense( license_json );
                    }
                }
            } catch( const Exception& e ) {
                if ( e.getErrCode() != DRM_Exit ) {
                    Error( e.what() );
                    f_asynch_error( std::string( e.what() ) );
                }
            } catch( const std::exception& e ) {
                Error( e.what() );
                f_asynch_error( std::string( e.what() ) );
            }
        });
    }

    void stopThread() {
        if ( !mThreadKeepAlive.valid() ) {
            Debug( "Background thread was not running" );
            return;
        }
        {
            std::lock_guard<std::mutex> lock( mThreadKeepAliveMtx );
            Debug( "Stop flag of thread is set" );
            mThreadStopRequest = true;
        }
        mThreadKeepAliveCondVar.notify_all();
        mThreadKeepAlive.get();
        Debug( "Background thread stopped" );
        {
            std::lock_guard<std::mutex> lock( mThreadKeepAliveMtx );
            Debug( "Stop flag of thread is reset" );
            mThreadStopRequest = false;
        }
    }

    void startSession() {
        Info( "Starting a new metering session..." );

        // Build start request message for new license
        Json::Value request_json = getMeteringStart();

        // Send request and receive new license
        Json::Value license_json = getLicense( request_json, mWSRequestTimeout, mWSRetryPeriodShort );
        setLicense( license_json );

        startLicenseContinuityThread();
    }

    void resumeSession() {
        Info( "Resuming DRM session..." );

        if ( isReadyForNewLicense() ) {

            // Create JSON license request
            Json::Value request_json = getMeteringWait();

            // Send license request to web service
            Json::Value license_json = getLicense( request_json, mWSRequestTimeout, mWSRetryPeriodShort );

            // Install license on DRM controller
            setLicense( license_json );
        }
        startLicenseContinuityThread();
    }

    void stopSession() {
        Info( "Stopping DRM session..." );

        // Stop background thread
        stopThread();

        // Get and send metering data to web service
        Json::Value request_json = getMeteringStop();

        // Send last metering information
        Json::Value license_json = getLicense( request_json, mWSRequestTimeout, mWSRetryPeriodShort );
        checkSessionIDFromWS( license_json );
        Info( "Session ID {} stopped and last metering data uploaded", mSessionID );

        /// Clear Session IS
        Debug( "Clearing session ID: {}", mSessionID );
        mSessionID = std::string("");
    }

    void pauseSession() {
        Info( "Pausing DRM session..." );
        stopThread();
        mSecurityStop = false;
    }

    ParameterKey findParameterKey( const std::string& key_string ) const {
        for ( auto const& it : mParameterKeyMap ) {
            if ( key_string == it.second ) {
                return it.first;
            }
        }
        Throw( DRM_BadArg, "Cannot find parameter: {}", key_string );
    }

    std::string findParameterString( const ParameterKey key_id ) const {
        std::map<ParameterKey, std::string>::const_iterator it;
        it = mParameterKeyMap.find( key_id );
        if ( it == mParameterKeyMap.end() )
            Throw( DRM_BadArg, "Cannot find parameter with ID: {}", key_id );
        return it->second;
    }

    Json::Value list_parameter_key() const {
        Json::Value node;
        for( int i=0; i<ParameterKey::ParameterKeyCount; i++ ) {
            ParameterKey e = static_cast<ParameterKey>( i );
            std::string keyStr = findParameterString( e );
            node.append( keyStr );
        }
        return node;
    }

    Json::Value dump_parameter_key() const {
        Json::Value node;
        for( int i=0; i<ParameterKey::dump_all; i++ ) {
            ParameterKey e = static_cast<ParameterKey>( i );
            std::string keyStr = findParameterString( e );
            node[ keyStr ] = Json::nullValue;
        }
        get( node );
        return node;
    }


public:
    Impl( const std::string& conf_file_path,
          const std::string& cred_file_path,
          ReadRegisterCallback f_user_read_register,
          WriteRegisterCallback f_user_write_register,
          AsynchErrorCallback f_user_asynch_error )
        : Impl( conf_file_path, cred_file_path )
    {
        if ( !f_user_read_register )
            Throw( DRM_BadArg, "Read register callback function must not be NULL" );
        if ( !f_user_write_register )
            Throw( DRM_BadArg, "Write register callback function must not be NULL" );
        if ( !f_user_asynch_error )
            Throw( DRM_BadArg, "Asynchronous error callback function must not be NULL" );
        f_read_register = f_user_read_register;
        f_write_register = f_user_write_register;
        f_asynch_error = f_user_asynch_error;
        initDrmInterface();
    }

    ~Impl() {
        if ( mSecurityStop ) {
            if ( isSessionRunning() ) {
                Debug( "Security stop triggered: stopping current session" );
                stopSession();
            }
        }
        stopThread();
        unlockDrmToInstance();
        uninitLog();
    }

    // Non copyable non movable as we create closure with "this"
    Impl( const Impl& ) = delete;
    Impl( Impl&& ) = delete;

    void activate( const bool& resume_session_request = false ) {
        TRY
            Debug( "Calling 'activate' with 'resume_session_request'={}", resume_session_request );

            bool isRunning = isSessionRunning();

            if ( mLicenseType == eLicenseType::NODE_LOCKED ) {
                // Install the node-locked license
                installNodelockedLicense();
                return;
            }
            if ( isDrmCtrlInNodelock() ) {
                Throw( DRM_BadUsage, "DRM Controller is locked in Node-Locked licensing mode: "
                                    "To use other modes you must reprogram the FPGA device." );
            }
            mSecurityStop = true;
            if ( isRunning && resume_session_request ) {
                resumeSession();
            } else {
                if ( isRunning && !resume_session_request ) {
                    Debug( "Session is already running but resume flag is {}: stopping this pending session",
                            resume_session_request );
                    try {
                        stopSession();
                    } catch( const Exception &e ) {
                        Debug( "Failed to stop pending session: {}", e.what() );
                    }
                }
                startSession();
            }
        CATCH_AND_THROW
    }

    void deactivate( const bool& pause_session_request = false ) {
        TRY
            Debug( "Calling 'deactivate' with 'pause_session_request'={}", pause_session_request );

            if ( mLicenseType == eLicenseType::NODE_LOCKED ) {
                return;
            }
            if ( !isSessionRunning() ) {
                Debug( "No session is currently running" );
                return;
            }
            if ( pause_session_request )
                pauseSession();
            else
                stopSession();
        CATCH_AND_THROW
    }

    void get( Json::Value& json_value ) const {
        TRY
            for( const std::string& key_str : json_value.getMemberNames() ) {
                const ParameterKey key_id = findParameterKey( key_str );
                Debug2( "Getting parameter '{}'", key_str );
                switch( key_id ) {
                    case ParameterKey::log_verbosity: {
                        int logVerbosity = static_cast<int>( sLogConsoleVerbosity );
                        json_value[key_str] = logVerbosity;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                logVerbosity );
                        break;
                    }
                    case ParameterKey::log_format: {
                        json_value[key_str] = sLogConsoleFormat;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                sLogConsoleFormat );
                        break;
                    }
                    case ParameterKey::log_file_verbosity: {
                        int logVerbosity = static_cast<int>( sLogFileVerbosity );
                        json_value[key_str] = logVerbosity;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                logVerbosity );
                        break;
                    }
                    case ParameterKey::log_file_format: {
                        json_value[key_str] = sLogFileFormat;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                sLogFileFormat );
                        break;
                    }
                    case ParameterKey::log_file_path: {
                        json_value[key_str] = sLogFilePath;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                sLogFilePath );
                        break;
                    }
                    case ParameterKey::log_file_type: {
                        json_value[key_str] = (int)sLogFileType;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               (int)sLogFileType );
                        break;
                    }
                    case ParameterKey::log_file_rotating_num: {
                        json_value[key_str] = (int)sLogFileRotatingNum;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               sLogFileRotatingNum );
                        break;
                    }
                    case ParameterKey::log_file_rotating_size: {
                        json_value[key_str] = (int)sLogFileRotatingSize;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               sLogFileRotatingSize );
                        break;
                    }
                    case ParameterKey::log_service_verbosity: {
                        int logVerbosity = static_cast<int>( sLogServiceVerbosity );
                        json_value[key_str] = logVerbosity;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               logVerbosity );
                        break;
                    }
                    case ParameterKey::log_service_format: {
                        json_value[key_str] = sLogServiceFormat;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               sLogServiceFormat );
                        break;
                    }
                    case ParameterKey::log_service_path: {
                        json_value[key_str] = sLogServicePath;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               sLogServicePath );
                        break;
                    }
                    case ParameterKey::log_service_type: {
                        json_value[key_str] = (int)sLogServiceType;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               (int)sLogServiceType );
                        break;
                    }
                    case ParameterKey::log_service_rotating_num: {
                        json_value[key_str] = (int)sLogServiceRotatingNum;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               sLogServiceRotatingNum );
                        break;
                    }
                    case ParameterKey::log_service_rotating_size: {
                        json_value[key_str] = (int)sLogServiceRotatingSize;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               sLogServiceRotatingSize );
                        break;
                    }
                    case ParameterKey::license_type: {
                        auto it = LicenseTypeStringMap.find( mLicenseType );
                        if ( it == LicenseTypeStringMap.end() )
                            Unreachable( "License_type '", (uint32_t)mLicenseType,
                                    "' is missing in LicenseTypeStringMap" ); //LCOV_EXCL_LINE
                        std::string license_type_str = it->second;
                        json_value[key_str] = license_type_str;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                license_type_str );
                        break;
                    }
                    case ParameterKey::license_duration: {
                        json_value[key_str] = mLicenseDuration;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                mLicenseDuration );
                        break;
                    }
                    case ParameterKey::num_activators: {
                        uint32_t nbActivators = 0;
                        getNumActivator( nbActivators );
                        json_value[key_str] = nbActivators;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                nbActivators );
                        break;
                    }
                    case ParameterKey::session_id: {
                        json_value[key_str] = mSessionID;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                                mSessionID );
                        break;
                    }
                    case ParameterKey::session_status: {
                        bool status = isSessionRunning();
                        json_value[key_str] = status;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               status );
                        break;
                    }
                    case ParameterKey::license_status: {
                        bool status = isLicenseActive();
                        json_value[key_str] = status;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               status );
                        break;
                    }
                    case ParameterKey::metered_data: {
#if ((JSONCPP_VERSION_MAJOR ) >= 1 and ((JSONCPP_VERSION_MINOR) > 7 or ((JSONCPP_VERSION_MINOR) == 7 and JSONCPP_VERSION_PATCH >= 5)))
                        uint64_t metered_data = 0;
#else
                        // No "int64_t" support with JsonCpp < 1.7.5
                        unsigned long long metered_data = 0;
#endif
                        metered_data = getMeteringData();
                        json_value[key_str] = metered_data;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               metered_data );
                        break;
                    }
                    case ParameterKey::nodelocked_request_file: {
                        if ( mLicenseType != eLicenseType::NODE_LOCKED ) {
                            json_value[key_str] = std::string("Not applicable");
                            Warning( "Parameter only available with Node-Locked licensing" );
                        } else {
                            json_value[key_str] = mNodeLockRequestFilePath;
                            Debug( "Get value of parameter '{}' (ID={}): Node-locked license request file is saved in {}",
                                    key_str, key_id, mNodeLockRequestFilePath );
                        }
                        break;
                    }
                    case ParameterKey::page_ctrlreg:
                    case ParameterKey::page_vlnvfile:
                    case ParameterKey::page_licfile:
                    case ParameterKey::page_tracefile:
                    case ParameterKey::page_meteringfile:
                    case ParameterKey::page_mailbox: {
                        std::string str = getDrmPage( key_id - ParameterKey::page_ctrlreg );
                        json_value[key_str] = str;
                        Debug( "Get value of parameter '{}' (ID={})", key_str, key_id );
                        Info( str );
                        break;
                    }
                    case ParameterKey::hw_report: {
                        std::string str = getDrmReport();
                        json_value[key_str] = str;
                        Debug( "Get value of parameter '{}' (ID={})", key_str, key_id );
                        Info( "Print HW report:\n{}", str );
                        break;
                    }
                    case ParameterKey::drm_frequency: {
                        json_value[key_str] = mFrequencyCurr;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mFrequencyCurr );
                        break;
                    }
                    case ParameterKey::drm_license_type: {
                        eLicenseType lic_type;
                        bool is_nodelock = isDrmCtrlInNodelock();
                        bool is_metering = isDrmCtrlInMetering();
                        if ( is_metering )
                            lic_type = eLicenseType::METERED;
                        else if ( is_nodelock )
                            lic_type = eLicenseType::NODE_LOCKED;
                        else
                            lic_type = eLicenseType::NONE;
                        auto it = LicenseTypeStringMap.find( lic_type );
                        std::string status = it->second;
                        json_value[key_str] = status;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               status );
                        break;
                    }
                    case ParameterKey::frequency_detection_threshold: {
                        json_value[key_str] = mFrequencyDetectionThreshold;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mFrequencyDetectionThreshold );
                        break;
                    }
                    case ParameterKey::frequency_detection_period: {
                        json_value[key_str] = mFrequencyDetectionPeriod;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mFrequencyDetectionPeriod );
                        break;
                    }
                    case ParameterKey::product_info: {
                        json_value[key_str] = mHeaderJsonRequest["product"];
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mHeaderJsonRequest["product"].toStyledString() );
                        break;
                    }
                    case ParameterKey::token_string: {
                        std::string token_string = getDrmWSClient().getTokenString();
                        json_value[key_str] = token_string;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               token_string );
                        break;
                    }
                    case ParameterKey::token_validity: {
                        uint32_t validity = getDrmWSClient().getTokenValidity();
                        json_value[key_str] = validity ;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               validity  );
                        break;
                    }
                    case ParameterKey::token_time_left: {
                        uint32_t time_left = getDrmWSClient().getTokenTimeLeft();
                        json_value[key_str] = time_left;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               time_left );
                        break;
                    }
                    case ParameterKey::mailbox_size: {
                        uint32_t mbSize = getMailboxSize() - (uint32_t)eMailboxOffset::MB_USER;
                        json_value[key_str] = mbSize;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mbSize );
                        break;
                    }
                    case ParameterKey::mailbox_data: {
                        uint32_t mbSize = getMailboxSize() - (uint32_t)eMailboxOffset::MB_USER;
                        std::vector<uint32_t> data_array = readMailbox( eMailboxOffset::MB_USER, mbSize );
                        for( const auto& val: data_array )
                            json_value[key_str].append( val );
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               json_value[key_str].toStyledString() );
                        break;
                    }
                    case ParameterKey::ws_retry_period_long: {
                        json_value[key_str] = mWSRetryPeriodLong;
                        Debug( "Get value of parameter '", key_str,
                                "' (ID=", key_id, "): ", mWSRetryPeriodLong );
                        break;
                    }
                    case ParameterKey::ws_retry_period_short: {
                        json_value[key_str] = mWSRetryPeriodShort;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mWSRetryPeriodShort );
                        break;
                    }
                    case ParameterKey::ws_request_timeout: {
                        json_value[key_str] = mWSRequestTimeout ;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               mWSRequestTimeout  );
                        break;
                    }
                    case ParameterKey::log_message_level: {
                        int msgLevel = static_cast<int>( mDebugMessageLevel );
                        json_value[key_str] = msgLevel;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               msgLevel );
                        break;
                    }
                    case ParameterKey::custom_field: {
                        uint32_t customField = readMailbox( eMailboxOffset::MB_CUSTOM_FIELD );
                        json_value[key_str] = customField;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               customField );
                        break;
                    }
                    case ParameterKey ::list_all: {
                        Json::Value list = list_parameter_key();
                        json_value[key_str] = list;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               list.toStyledString() );
                        break;
                    }
                    case ParameterKey::dump_all: {
                        Json::Value list = dump_parameter_key();
                        json_value[key_str] = list;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               list.toStyledString() );
                        break;
                    }
                    case ParameterKey::ParameterKeyCount: {
                        uint32_t count = static_cast<int>( ParameterKeyCount );
                        json_value[key_str] = count;
                        Debug( "Get value of parameter '{}' (ID={}): {}", key_str, key_id,
                               count );
                        break;
                    }
                    default: {
                        Throw( DRM_BadArg, "Parameter '{}' cannot be read", key_str );
                        break;
                    }
                }
            }
        CATCH_AND_THROW
    }

    void get( std::string& json_string ) const {
        TRY
            Debug2( "Calling 'get' with in/out string: {}", json_string );
            Json::Value root = parseJsonString( json_string );
            get( root );
            json_string = root.toStyledString();
        CATCH_AND_THROW
    }

    template<typename T> T get( const ParameterKey /*key_id*/ ) const {
        Unreachable( "Default template for get function" ); //LCOV_EXCL_LINE
    }

    void set( const Json::Value& json_value ) {
        TRY
            for( Json::ValueConstIterator it = json_value.begin() ; it != json_value.end() ; it++ ) {
                std::string key_str = it.key().asString();
                const ParameterKey key_id = findParameterKey( key_str );
                switch( key_id ) {
                    case ParameterKey::log_verbosity: {
                        int verbosityInt = (*it).asInt();
                        sLogConsoleVerbosity = static_cast<spdlog::level::level_enum>( verbosityInt );
                        sLogger->sinks()[0]->set_level( sLogConsoleVerbosity );
                        if ( sLogConsoleVerbosity < sLogger->level() )
                            sLogger->set_level( sLogConsoleVerbosity );
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                verbosityInt );
                        break;
                    }
                    case ParameterKey::log_format: {
                        std::string logFormat = (*it).asString();
                        sLogger->sinks()[0]->set_pattern( logFormat );
                        sLogConsoleFormat = logFormat;
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                sLogConsoleFormat );
                        break;
                    }
                    case ParameterKey::log_file_verbosity: {
                        int verbosityInt = (*it).asInt();
                        sLogFileVerbosity = static_cast<spdlog::level::level_enum>( verbosityInt );
                        sLogger->sinks()[1]->set_level( sLogFileVerbosity );
                        if ( sLogFileVerbosity < sLogger->level() )
                            sLogger->set_level( sLogFileVerbosity );
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               verbosityInt);
                        break;
                    }
                    case ParameterKey::log_file_format: {
                        sLogFileFormat = (*it).asString();
                        if ( sLogger->sinks().size() > 1 ) {
                            sLogger->sinks()[1]->set_pattern( sLogFileFormat );
                        }
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               sLogFileFormat );
                        break;
                    }
                    case ParameterKey::log_service_verbosity: {
                        int verbosityInt = (*it).asInt();
                        sLogServiceVerbosity = static_cast<spdlog::level::level_enum>( verbosityInt );
                        if ( sLogger->sinks().size() == 3 ) {
                            sLogger->sinks()[2]->set_level( sLogServiceVerbosity );
                            if ( sLogServiceVerbosity < sLogger->level() )
                                sLogger->set_level( sLogServiceVerbosity );
                        }
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id, verbosityInt );
                        break;
                    }
                    case ParameterKey::log_service_format: {
                        sLogServiceFormat = (*it).asString();
                        if ( sLogger->sinks().size() == 3 ) {
                            sLogger->sinks()[2]->set_pattern( sLogServiceFormat );
                        }
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               sLogServiceFormat );
                        break;
                    }
                    case ParameterKey::log_service_path: {
                        if ( sLogger->sinks().size() < 3 ) {
                            sLogServicePath = (*it).asString();
                            Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                   sLogServicePath );
                        } else {
                            Warning( "A service logging is already in use: cannot change its settings" );
                        }
                        break;
                    }
                    case ParameterKey::log_service_type: {
                        if ( sLogger->sinks().size() < 3 ) {
                            int logType = (*it).asInt();
                            sLogServiceType = static_cast<eLogFileType>( logType  );
                            Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                   (int)sLogServiceType );
                        } else {
                            Warning( "A service logging is already in use" );
                        }
                        break;
                    }
                    case ParameterKey::log_service_rotating_size: {
                        if ( sLogger->sinks().size() < 3 ) {
                            sLogServiceRotatingSize = (*it).asUInt();
                            Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                   sLogServiceRotatingSize );
                        } else {
                            Warning( "A service logging is already in use" );
                        }
                        break;
                    }
                    case ParameterKey::log_service_rotating_num: {
                        if ( sLogger->sinks().size() < 3 ) {
                            sLogServiceRotatingNum = (*it).asUInt();
                            Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                   sLogServiceRotatingNum );
                        } else {
                            Warning( "A service logging is already in use" );
                        }
                        break;
                    }
                    case ParameterKey::log_service_create: {
                        if ( sLogger->sinks().size() < 3 ) {
                            std::string dummy = (*it).asString();
                            createFileLog( sLogServicePath, sLogServiceType, sLogServiceVerbosity,
                                    sLogServiceFormat, sLogServiceRotatingSize, sLogServiceRotatingNum );
                            Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                                   dummy );
                        } else {
                            Warning( "A service logging is already in use" );
                        }
                        break;
                    }

                    case ParameterKey::frequency_detection_threshold: {
                        mFrequencyDetectionThreshold = (*it).asDouble();
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               mFrequencyDetectionThreshold );
                        break;
                    }
                    case ParameterKey::frequency_detection_period: {
                        mFrequencyDetectionPeriod = (*it).asUInt();
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               mFrequencyDetectionPeriod );
                        break;
                    }
                    case ParameterKey::custom_field: {
                        uint32_t customField = (*it).asUInt();
                        writeMailbox( eMailboxOffset::MB_CUSTOM_FIELD, customField );
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               customField );
                        break;
                    }
                    case ParameterKey::mailbox_data: {
                        if ( !(*it).isArray() )
                            Throw( DRM_BadArg, "Value must be an array of integers" );
                        std::vector<uint32_t> data_array;
                        for( Json::ValueConstIterator itr = (*it).begin(); itr != (*it).end(); itr++ )
                            data_array.push_back( (*itr).asUInt() );
                        writeMailbox( eMailboxOffset::MB_USER, data_array );
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               (*it).toStyledString());
                        break;
                    }
                    case ParameterKey::ws_retry_period_long: {
                        uint32_t retry_period = (*it).asUInt();
                        if ( retry_period <= mWSRetryPeriodShort )
                            Throw( DRM_BadArg,
                                    "ws_retry_period_long ({}) must be greater than ws_retry_period_short ({})",
                                    retry_period, mWSRetryPeriodShort );
                        mWSRetryPeriodLong = retry_period;
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               mWSRetryPeriodLong );
                        break;
                    }
                    case ParameterKey::ws_retry_period_short: {
                        uint32_t retry_period = (*it).asUInt();
                        if ( mWSRetryPeriodLong <= retry_period )
                            Throw( DRM_BadArg,
                                    "ws_retry_period_long ({}) must be greater than ws_retry_period_short ({})",
                                    mWSRetryPeriodLong, retry_period );
                        mWSRetryPeriodShort = retry_period;
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               mWSRetryPeriodShort );
                        break;
                    }
                    case ParameterKey::ws_request_timeout: {
                        mWSRequestTimeout  = (*it).asUInt();
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               mWSRequestTimeout  );
                        if ( mWSRequestTimeout == 0 )
                            Throw( DRM_BadArg, "ws_request_timeout must not be 0");
                        break;
                    }
                    case ParameterKey::trigger_async_callback: {
                        std::string custom_msg = (*it).asString();
                        Exception e( DRM_Debug, custom_msg );
                        f_asynch_error( e.what() );
                        Debug( "Set parameter '{}' (ID={}) to value: {}", key_str, key_id,
                               custom_msg );
                        break;
                    }
                    case ParameterKey::bad_product_id: {
                        Debug( "Set parameter '{}' (ID={}) to random value", key_str, key_id );
                        mHeaderJsonRequest["product"]["name"] = "BAD_NAME_JUST_FOR_TEST";
                        break;
                    }
                    case ParameterKey::bad_oauth2_token: {
                        Debug( "Set parameter '{}' (ID={}) to random value", key_str, key_id );
                        getDrmWSClient().setOAuth2token( "BAD_TOKEN" );
                        break;
                    }
                    case ParameterKey::log_message_level: {
                        int message_level = (*it).asInt();
                        if ( ( message_level < spdlog::level::trace)
                          || ( message_level > spdlog::level::off) )
                            Throw( DRM_BadArg, "log_message_level ({}) is out of range [{:d}:{:d}]",
                                    message_level, (int)spdlog::level::trace, (int)spdlog::level::off );
                        mDebugMessageLevel = static_cast<spdlog::level::level_enum>( message_level );
                        Debug( "Set parameter '{}' (ID={}) to value {}", key_str, key_id,
                                message_level );
                        break;
                    }
                    case ParameterKey::log_message: {
                        std::string custom_msg = (*it).asString();
                        SPDLOG_LOGGER_CALL( sLogger, (spdlog::level::level_enum)mDebugMessageLevel, custom_msg);
                        break;
                    }
                    default:
                        Throw( DRM_BadArg, "Parameter '{}' cannot be overwritten", key_str );
                }
            }
        CATCH_AND_THROW
    }

    void set( const std::string& json_string ) {
        TRY
            Debug2( "Calling 'set' with in/out string: {}", json_string );
            Json::Value root = parseJsonString( json_string );
            set( root );
        CATCH_AND_THROW
    }

    template<typename T> void set( const ParameterKey /*key_id*/, const T& /*value*/ ) {}

};

/*************************************/
// DrmManager::Impl class definition
/*************************************/

#define IMPL_GET_BODY \
    Json::Value json_value; \
    std::string key_str = findParameterString( key_id ); \
    json_value[key_str] = Json::nullValue; \
    get( json_value );

template<> std::string DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    if ( json_value[key_str].isString() )
        return json_value[key_str].asString();
    return json_value[key_str].toStyledString();
}

template<> bool DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asBool();
}

template<> int32_t DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asInt();
}

template<> uint32_t DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asUInt();
}

template<> int64_t DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asInt64();
}

template<> uint64_t DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asUInt64();
}

template<> float DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asFloat();
}

template<> double DrmManager::Impl::get( const ParameterKey key_id ) const {
    IMPL_GET_BODY
    return json_value[key_str].asDouble();
}

#define IMPL_SET_BODY \
    Json::Value json_value; \
    std::string key_str = findParameterString( key_id ); \
    json_value[key_str] = value; \
    set( json_value );


template<> void DrmManager::Impl::set( const ParameterKey key_id, const std::string& value ) {
    IMPL_SET_BODY
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const bool& value ) {
    IMPL_SET_BODY
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const int32_t& value ) {
    IMPL_SET_BODY
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const uint32_t& value ) {
    IMPL_SET_BODY
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const int64_t& value ) {
    Json::Value json_value;
    std::string key_str = findParameterString( key_id );
    json_value[key_str] = Json::Int64( value );
    set( json_value );
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const uint64_t& value ) {
    Json::Value json_value;
    std::string key_str = findParameterString( key_id );
    json_value[key_str] = Json::UInt64( value );
    set( json_value );
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const float& value ) {
    IMPL_SET_BODY
}

template<> void DrmManager::Impl::set( const ParameterKey key_id, const double& value ) {
    IMPL_SET_BODY
}


/*************************************/
// DrmManager class definition
/*************************************/

DrmManager::DrmManager( const std::string& conf_file_path,
                    const std::string& cred_file_path,
                    ReadRegisterCallback read_register,
                    WriteRegisterCallback write_register,
                    AsynchErrorCallback async_error )
    : pImpl( new Impl( conf_file_path, cred_file_path, read_register, write_register, async_error ) ) {
}

DrmManager::~DrmManager() {
    delete pImpl;
    pImpl = nullptr;
}


void DrmManager::activate( const bool& resume_session ) {
    pImpl->activate( resume_session );
}

void DrmManager::deactivate( const bool& pause_session ) {
    pImpl->deactivate( pause_session );
}

void DrmManager::get( Json::Value& json_value ) const {
    pImpl->get( json_value );
}

void DrmManager::get( std::string& json_string ) const {
    pImpl->get( json_string );
}

template<typename T> T DrmManager::get( const ParameterKey key ) const {
    return pImpl->get<T>( key );
}

template<> std::string DrmManager::get( const ParameterKey key ) const { return pImpl->get<std::string>( key ); }
template<> bool DrmManager::get( const ParameterKey key ) const { return pImpl->get<bool>( key ); }
template<> int32_t DrmManager::get( const ParameterKey key ) const { return pImpl->get<int32_t>( key ); }
template<> uint32_t DrmManager::get( const ParameterKey key ) const { return pImpl->get<uint32_t>( key ); }
template<> int64_t DrmManager::get( const ParameterKey key ) const { return pImpl->get<int64_t>( key ); }
template<> uint64_t DrmManager::get( const ParameterKey key ) const { return pImpl->get<uint64_t>( key ); }
template<> float DrmManager::get( const ParameterKey key ) const { return pImpl->get<float>( key ); }
template<> double DrmManager::get( const ParameterKey key ) const { return pImpl->get<double>( key ); }

void DrmManager::set( const std::string& json_string ) {
    pImpl->set( json_string );
}

void DrmManager::set( const Json::Value& json_value ) {
    pImpl->set( json_value );
}

template<typename T>
void DrmManager::set( const ParameterKey key, const T& value ) {
    pImpl->set<T>( key, value );
}

template<> void DrmManager::set( const ParameterKey key, const std::string& value ) { pImpl->set<std::string>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const bool& value ) { pImpl->set<bool>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const int32_t& value ) { pImpl->set<int32_t>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const uint32_t& value ) { pImpl->set<uint32_t>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const int64_t& value ) { pImpl->set<int64_t>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const uint64_t& value ) { pImpl->set<uint64_t>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const float& value ) { pImpl->set<float>( key, value ); }
template<> void DrmManager::set( const ParameterKey key, const double& value ) { pImpl->set<double>( key, value ); }

}
}
