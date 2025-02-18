/* =====================================================================================
 * Keyboard daemon.
 *
 * Copyright (C) 2018 Jonas Møller (no) <jonasmo441@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * =====================================================================================
 */

#include <fstream>
#include <iostream>
#include <string>
#include <chrono>

extern "C" {
    #include <syslog.h>
    #include <grp.h>
}

#include "KBDDaemon.hpp"
#include "CSV.hpp"
#include "utils.hpp"
#include "Daemon.hpp"
#include "Permissions.hpp"

// #undef DANGER_DANGER_LOG_KEYS
// #define DANGER_DANGER_LOG_KEYS 1

#if DANGER_DANGER_LOG_KEYS
    #warning "Currently logging keypresses"
    #warning "DANGER_DANGER_LOG_KEYS should **only** be enabled while debugging."
#endif

using namespace std;
using namespace Permissions;

constexpr int FSW_MAX_WAIT_PERMISSIONS_US = 5 * 1000000;

KBDDaemon::KBDDaemon() :
    kbd_com("/var/lib/hawck-input/kbd.sock")
{
    initPassthrough();
}

void KBDDaemon::addDevice(const std::string& device) {
    kbds.push_back(new Keyboard(device.c_str()));
}

KBDDaemon::~KBDDaemon() {
    for (Keyboard *kbd : kbds)
        delete kbd;
}

void KBDDaemon::unloadPassthrough(std::string path) {
    if (key_sources.find(path) != key_sources.end()) {
        auto vec = key_sources[path];
        for (int code : *vec)
            passthrough_keys.erase(code);
        delete vec;
        key_sources.erase(path);

        syslog(LOG_INFO, "Removing passthrough keys from: %s", path.c_str());

        // Re-add keys
        for (const auto &[_, vec] : key_sources)
            for (int code : *vec)
                passthrough_keys.insert(code);
    }
}

void KBDDaemon::loadPassthrough(std::string rel_path) {
    try {
        // The CSV file is being reloaded after a change, remove the old keys.
        char *rpath = realpath(rel_path.c_str(), nullptr);
        if (rpath == nullptr) {
            throw SystemError("Error in realpath() unable to get path for: " + rel_path);
        }
        string path(rpath);
        free(rpath);

        unloadPassthrough(path);

        CSV csv(path);
        auto cells = mkuniq(csv.getColCells("key_code"));
        auto cells_i = mkuniq(new vector<int>());
        for (auto *code_s : *cells) {
            int i;
            try {
                i = stoi(*code_s);
            } catch (const std::exception &e) {
                continue;
            }
            if (i >= 0) {
                passthrough_keys.insert(i);
                cells_i->push_back(i);
            }
        }
        key_sources[path] = cells_i.release();
        keys_fsw.add(path);
        syslog(LOG_INFO, "Loaded passthrough keys from: %s", path.c_str());
    } catch (const CSV::CSVError &e) {
        syslog(LOG_ERR, "CSV parse error in '%s': %s",
               rel_path.c_str(), e.what());
    } catch (const SystemError &e) {
        syslog(LOG_ERR, "Unable to load csv data from '%s': %s",
               rel_path.c_str(), e.what());
    }
}

void KBDDaemon::loadPassthrough(FSEvent *ev) {
    unsigned perm = ev->stbuf.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);

    // Require that the file permission mode is 644 and that the file is owned
    // by the daemon user.
    if (perm == 0644 && ev->stbuf.st_uid == getuid()) {
        loadPassthrough(ev->path);
    } else {
        auto [grp, grpbuf] = getgroup(ev->stbuf.st_uid);
        auto perm_str = fmtPermissions(ev->stbuf);
        syslog(LOG_ERR, "Invalid permissions for '%s', require rw-r--r-- hawck-input:*, "
                        "but was: %s",
               ev->path.c_str(), perm_str.c_str());
    }
}

void KBDDaemon::initPassthrough() {
    auto files = mkuniq(keys_fsw.addFrom(data_dirs["keys"]));
    for (auto &file : *files)
        loadPassthrough(&file);
}

void KBDDaemon::updateAvailableKBDs() {
    available_kbds.clear();
    for (auto &kbd : kbds)
        if (!kbd->isDisabled())
            available_kbds.push_back(kbd);
}

void KBDDaemon::run() {
    KBDAction action;

    for (auto& kbd : kbds) {
        syslog(LOG_INFO, "Attempting to get lock on device: %s @ %s",
               kbd->getName().c_str(), kbd->getPhys().c_str());
        kbd->lock();
    }

    updateAvailableKBDs();

    keys_fsw.begin([this](FSEvent &ev) {
                       lock_guard<mutex> lock(passthrough_keys_mtx);
                       syslog(LOG_INFO, "kbd file change on: %s", ev.path.c_str());
                       if (ev.mask & IN_DELETE_SELF)
                           unloadPassthrough(ev.path);
                       else if (ev.mask & (IN_CREATE | IN_MODIFY))
                           loadPassthrough(&ev);
                       return true;
                   });

    input_fsw.add("/dev/input");
    input_fsw.setWatchDirs(true);
    input_fsw.setAutoAdd(false);

    gid_t input_gid; {
        auto [grp, grpbuf] = getgroup("input");
        input_gid = grp->gr_gid;
    }


    input_fsw.begin([this, input_gid](FSEvent &ev) {
                        // Don't react to the directory itself.
                        if (ev.path == "/dev/input")
                            return true;

                        syslog(LOG_INFO, "Input device hotplug event on: %s",
                               ev.path.c_str());

                        lock_guard<mutex> lock(pulled_kbds_mtx);

                        for (auto it = pulled_kbds.begin(); it != pulled_kbds.end(); it++) {
                            auto kbd = *it;

                            int wait_inc_us = 100;
                            int wait_time = 0;
                            // Loop until the file has the correct permissions,
                            // when immediately added /dev/input/* files seem
                            // to be owned by root:root or by root:input with
                            // restrictive permissions.
                            // We expect it to be root:input with the input group
                            // being able to read and write.
                            struct stat stbuf;
                            unsigned grp_perm;
                            int ret;
                            do {
                                usleep(wait_inc_us);
                                ret = stat(ev.path.c_str(), &stbuf);
                                grp_perm = stbuf.st_mode & S_IRWXG;

                                // Check if it is a character device, test is
                                // done here because permissions might not allow
                                // for even stat()ing the file.
                                if (ret != -1 && !S_ISCHR(stbuf.st_mode)) {
                                    // Not a character device, return
                                    syslog(LOG_WARNING, "File %s is not a character device",
                                           ev.path.c_str());
                                    return true;
                                }

                                if ((wait_time += wait_inc_us) > FSW_MAX_WAIT_PERMISSIONS_US) {
                                    syslog(LOG_ERR,
                                           "Could not aquire permissions rw with group input on '%s'",
                                           ev.path.c_str());
                                    // Skip this file
                                    return true;
                                }
                            } while (ret != -1 && !(4 & grp_perm && grp_perm & 2) && stbuf.st_gid != input_gid);

                            if (kbd->isMe(ev.path.c_str())) {
                                syslog(LOG_INFO,
                                       "Keyboard was plugged in: %s",
                                       kbd->getName().c_str());
                                kbd->reset(ev.path.c_str());
                                kbd->lock();
                                {
                                    lock_guard<mutex> lock(available_kbds_mtx);
                                    available_kbds.push_back(kbd);
                                }
                                pulled_kbds.erase(it);
                                break;
                            }
                        }
                        return true;
                    });

    Keyboard *kbd = nullptr;
    for (;;) {
        bool had_key = false;
        action.done = 0;
        try {
            available_kbds_mtx.lock();
            vector<Keyboard*> kbds(available_kbds);
            available_kbds_mtx.unlock();
            int idx = kbdMultiplex(kbds, 64);
            if (idx != -1) {
                kbd = kbds[idx];
                kbd->get(&action.ev);

                // Throw away the key if the keyboard isn't locked yet.
                if (kbd->getState() == KBDState::LOCKED)
                    had_key = true;
                // Always lock unlocked keyboards.
                else if (kbd->getState() == KBDState::OPEN)
                    kbd->lock();
            }
        } catch (const KeyboardError &e) {
            // Disable the keyboard,
            syslog(LOG_ERR,
                   "Read error on keyboard, assumed to be removed: %s",
                   kbd->getName().c_str());
            kbd->disable();
            {
                lock_guard<mutex> lock(available_kbds_mtx);
                auto pos_it = find(available_kbds.begin(),
                                   available_kbds.end(),
                                   kbd);
                available_kbds.erase(pos_it);
            }
            lock_guard<mutex> lock(pulled_kbds_mtx);
            pulled_kbds.push_back(kbd);
        }

        if (!had_key)
            continue;

        bool is_passthrough; {
            lock_guard<mutex> lock(passthrough_keys_mtx);
            is_passthrough = passthrough_keys.count(action.ev.code);
        }

        // Check if the key is listed in the passthrough set.
        if (is_passthrough) {
            input_event orig_ev = action.ev;

            // Pass key to Lua executor
            try {
                kbd_com.send(&action);

                // Receive keys to emit from the macro daemon.
                int count = 0;
                for (;; count++) {
                    kbd_com.recv(&action, timeout);
                    if (action.done)
                        break;
                    udev.emit(&action.ev);
                }
                // Flush received keys and continue on.
                udev.flush();
                // Skip emmision of the original key if everything went OK
                if (count == 0)
                    cout << "MacroD swallowed event" << endl;
                continue;
            } catch (const SocketError &e) {
                lock_guard<mutex> lock(available_kbds_mtx);

                cout << "Resetting connection ..." << endl;

                udev.emit(&orig_ev);
                udev.upAll();
                udev.flush();
                udev.upAll();
                udev.flush();

                // Unlock all keyboards so that the user can actually type.
                for (auto& kbd : available_kbds)
                    try {
                        syslog(LOG_INFO, "Unlocking keyboard due to error: \"%s\" @ %s",
                               kbd->getName().c_str(), kbd->getPhys().c_str());
                        kbd->unlock();
                    } catch (const KeyboardError &e) {
                        syslog(LOG_ERR, "Unable to unlock keyboard: %s", kbd->getName().c_str());
                        kbd->disable();
                    }

                syslog(LOG_CRIT, "Unable to communicate with MacroD, reconnecting ...");

                // Reconnect.
                kbd_com.recon();

                // Lock keyboards
                for (auto& kbd : available_kbds)
                    try {
                        kbd->lock();
                    } catch (const KeyboardError &e) {
                        // Report the error and continue, further keyboard
                        // errors will be caught in kbd->get() later on.
                        syslog(LOG_ERR, "Unable to lock keyboard: %s", kbd->getName().c_str());
                    }

                // Skip the received event
                continue;
            }
        }

        udev.emit(&action.ev);
        udev.flush();
    }
}

void KBDDaemon::setEventDelay(int delay) {
    udev.setEventDelay(delay);
}

