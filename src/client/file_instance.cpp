/*
 *  Copyright (c) 2020 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * File Created: Tuesday, 25th September 2018 4:58:20 pm
 * Author: tongguangxun
 */

#include "src/client/file_instance.h"

#include <butil/endpoint.h>
#include <glog/logging.h>
#include <utility>

#include "proto/nameserver2.pb.h"
#include "proto/topology.pb.h"
#include "src/client/iomanager4file.h"
#include "src/client/mds_client.h"
#include "src/client/metacache.h"
#include "src/client/request_scheduler.h"
#include "src/client/request_sender_manager.h"
#include "src/common/timeutility.h"

namespace curve {
namespace client {

using curve::client::ClientConfig;
using curve::common::TimeUtility;
using curve::mds::SessionStatus;

FileInstance::FileInstance()
    : finfo_(),
      fileopt_(),
      mdsclient_(nullptr),
      leaseExecutor_(),
      iomanager4file_(),
      readonly_(false) {}

bool FileInstance::Initialize(const std::string& filename,
                              std::shared_ptr<MDSClient> mdsclient,
                              const UserInfo_t& userinfo,
                              const FileServiceOption& fileservicopt,
                              bool readonly) {
    readonly_ = readonly;
    fileopt_ = fileservicopt;
    bool ret = false;
    do {
        if (!userinfo.Valid()) {
            LOG(ERROR) << "userinfo not valid!";
            break;
        }

        if (mdsclient == nullptr) {
            LOG(ERROR) << "mdsclient pointer is null!";
            break;
        }

        finfo_.userinfo = userinfo;
        mdsclient_ = std::move(mdsclient);

        finfo_.fullPathName = filename;

        if (!iomanager4file_.Initialize(filename, fileopt_.ioOpt,
                                        mdsclient_.get())) {
            LOG(ERROR) << "Init io context manager failed, filename = "
                       << filename;
            break;
        }

        iomanager4file_.UpdateFileInfo(finfo_);

        leaseExecutor_.reset(new (std::nothrow) LeaseExecutor(
            fileopt_.leaseOpt, finfo_.userinfo, mdsclient_.get(),
            &iomanager4file_));
        if (CURVE_UNLIKELY(leaseExecutor_ == nullptr)) {
            LOG(ERROR) << "Allocate LeaseExecutor failed, filename = "
                       << filename;
            break;
        }

        ret = true;
    } while (0);

    return ret;
}

void FileInstance::UnInitialize() {
    iomanager4file_.UnInitialize();

    // release the ownership of mdsclient
    mdsclient_.reset();
}

int FileInstance::Read(char* buf, off_t offset, size_t length) {
    return iomanager4file_.Read(buf, offset, length, mdsclient_.get());
}

int FileInstance::Write(const char* buf, off_t offset, size_t len) {
    if (readonly_) {
        DVLOG(9) << "open with read only, do not support write!";
        return -1;
    }
    return iomanager4file_.Write(buf, offset, len, mdsclient_.get());
}

int FileInstance::AioRead(CurveAioContext* aioctx, UserDataType dataType) {
    return iomanager4file_.AioRead(aioctx, mdsclient_.get(), dataType);
}

int FileInstance::AioWrite(CurveAioContext* aioctx, UserDataType dataType) {
    if (readonly_) {
        DVLOG(9) << "open with read only, do not support write!";
        return -1;
    }
    return iomanager4file_.AioWrite(aioctx, mdsclient_.get(), dataType);
}

// 两种场景会造成在Open的时候返回LIBCURVE_ERROR::FILE_OCCUPIED
// 1. 强制重启qemu不会调用close逻辑，然后启动的时候原来的文件sessio还没过期.
//    导致再次去发起open的时候，返回被占用，这种情况可以通过load sessionmap
//    拿到已有的session，再去执行refresh。
// 2. 由于网络原因，导致open rpc超时，然后再去重试的时候就会返回FILE_OCCUPIED
//    这时候当前还没有成功打开，所以还没有存储该session信息，所以无法通过refresh
//    再去打开，所以这时候需要获取mds一侧session lease时长，然后在client这一侧
//    等待一段时间再去Open，如果依然失败，就向上层返回失败。
int FileInstance::Open(const std::string& filename,
                       const UserInfo& userinfo,
                       std::string* sessionId) {
    LeaseSession_t  lease;
    int ret = LIBCURVE_ERROR::FAILED;

    ret = mdsclient_->OpenFile(filename, finfo_.userinfo, &finfo_, &lease);
    if (ret == LIBCURVE_ERROR::OK) {
        ret = leaseExecutor_->Start(finfo_, lease) ? LIBCURVE_ERROR::OK
                                                   : LIBCURVE_ERROR::FAILED;
        if (nullptr != sessionId) {
            sessionId->assign(lease.sessionID);
        }
    }
    return -ret;
}

int FileInstance::ReOpen(const std::string& filename,
                         const std::string& sessionId,
                         const UserInfo& userInfo,
                         std::string* newSessionId) {
    return Open(filename, userInfo, newSessionId);
}

int FileInstance::GetFileInfo(const std::string& filename, FInfo_t* fi) {
    LIBCURVE_ERROR ret = mdsclient_->GetFileInfo(filename, finfo_.userinfo, fi);
    return -ret;
}

int FileInstance::Close() {
    if (readonly_) {
        LOG(INFO) << "close read only file!" << finfo_.fullPathName;
        return 0;
    }

    if (leaseExecutor_ != nullptr) {
        leaseExecutor_->Stop();
        leaseExecutor_.reset();
    }

    LIBCURVE_ERROR ret =
        mdsclient_->CloseFile(finfo_.fullPathName, finfo_.userinfo, "");
    return -ret;
}

FileInstance* FileInstance::NewInitedFileInstance(
    const FileServiceOption& fileServiceOption,
    std::shared_ptr<MDSClient> mdsClient,
    const std::string& filename,
    const UserInfo& userInfo,
    bool readonly) {
    FileInstance* instance = new (std::nothrow) FileInstance();
    if (instance == nullptr) {
        LOG(ERROR) << "Create FileInstance failed, filename: " << filename;
        return nullptr;
    }

    bool ret = instance->Initialize(filename, std::move(mdsClient), userInfo,
                                    fileServiceOption, readonly);
    if (!ret) {
        LOG(ERROR) << "FileInstance initialize failed"
                   << ", filename = " << filename
                   << ", ower = " << userInfo.owner
                   << ", readonly = " << readonly;
        delete instance;
        return nullptr;
    }

    return instance;
}

FileInstance* FileInstance::Open4Readonly(const FileServiceOption& opt,
                                          std::shared_ptr<MDSClient> mdsclient,
                                          const std::string& filename,
                                          const UserInfo& userInfo) {
    FileInstance* instance = FileInstance::NewInitedFileInstance(
        opt, std::move(mdsclient), filename, userInfo, true);
    if (instance == nullptr) {
        LOG(ERROR) << "NewInitedFileInstance failed, filename = " << filename;
        return nullptr;
    }

    FInfo fileInfo;
    int ret = instance->GetFileInfo(filename, &fileInfo);
    if (ret != 0) {
        LOG(ERROR) << "Get file info failed!";
        instance->UnInitialize();
        delete instance;
        return nullptr;
    }

    fileInfo.userinfo = userInfo;
    fileInfo.fullPathName = filename;
    instance->GetIOManager4File()->UpdateFileInfo(fileInfo);

    return instance;
}

}   // namespace client
}   // namespace curve
