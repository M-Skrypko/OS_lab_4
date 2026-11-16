#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>

constexpr size_t MAX_MESSAGE_LENGTH = 20;
constexpr size_t MESSAGE_BUFFER_SIZE = MAX_MESSAGE_LENGTH * 4;
constexpr int MAX_RECORDS = 1000;
constexpr int MAX_SENDERS = 50;

const std::string MUTEX_NAME = "Local\\Lab4_FileMutex_Skrypko";
const std::string SEM_EMPTY_NAME = "Local\\Lab4_SemEmpty_Skrypko";
const std::string SEM_FILLED_NAME = "Local\\Lab4_SemFilled_Skrypko";
const std::string EVENT_READY_PREFIX = "Local\\Lab4_SenderReady_Skrypko_";

#pragma pack(push, 1)
struct QueueHeader {
    int head;
    int tail;
    int capacity;
    int count;
};

struct MessageSlot {
    char text[MESSAGE_BUFFER_SIZE];
};
#pragma pack(pop)

class UniqueHandle {
private:
    HANDLE handle_;
public:
    explicit UniqueHandle(HANDLE h = nullptr) noexcept : handle_(h) {}
    ~UniqueHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
};

inline std::string WideToUtf8(const wchar_t* text, int length) {
    if (text == nullptr || length <= 0) {
        return std::string();
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::string();
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, &result[0], size, nullptr, nullptr);
    return result;
}

inline std::string GetLastErrorAsString(DWORD errorCode = ::GetLastError()) {
    if (errorCode == 0) {
        return "ошибка не определена";
    }

    LPWSTR buffer = nullptr;
    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::string message;
    if (length > 0 && buffer != nullptr) {
        while (length > 0 && (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' || buffer[length - 1] == L' ')) {
            --length;
        }
        message = WideToUtf8(buffer, static_cast<int>(length));
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    if (!message.empty()) {
        message += " ";
    }
    return message + "(Код: " + std::to_string(errorCode) + ")";
}

class MutexLock {
private:
    HANDLE mutex_;
public:
    explicit MutexLock(HANDLE mutex) : mutex_(mutex) {
        const DWORD result = WaitForSingleObject(mutex_, INFINITE);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
            throw std::runtime_error("Ошибка ожидания мьютекса: " + GetLastErrorAsString());
        }
    }
    ~MutexLock() {
        ReleaseMutex(mutex_);
    }
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
};

inline bool IsProcessFinished(HANDLE process) {
    return WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
}

inline std::string MakeObjectName(const std::string& baseName, DWORD receiverPid) {
    return baseName + "_" + std::to_string(receiverPid);
}

inline std::string Trim(const std::string& text) {
    const char* const whitespace = " \t\r\n";
    const size_t first = text.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = text.find_last_not_of(whitespace);
    return text.substr(first, last - first + 1);
}

inline std::string ReadLine(const std::string& prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
        throw std::runtime_error("Ввод прерван: поток ввода закрыт.");
    }
    return line;
}

inline int ReadIntSafe(const std::string& prompt, int minVal, int maxVal) {
    while (true) {
        const std::string line = Trim(ReadLine(prompt));
        try {
            size_t parsed = 0;
            const long long value = std::stoll(line, &parsed);
            if (parsed == line.size() && value >= minVal && value <= maxVal) {
                return static_cast<int>(value);
            }
        }
        catch (const std::exception&) {
        }
        std::cout << "[Ошибка] Введите целое число от " << minVal << " до " << maxVal << ".\n";
    }
}

inline size_t CountUtf8Chars(const std::string& text) {
    size_t count = 0;
    for (const unsigned char symbol : text) {
        if ((symbol & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

inline bool IsHeaderValid(const QueueHeader& header) {
    return header.capacity > 0
        && header.head >= 0 && header.head < header.capacity
        && header.tail >= 0 && header.tail < header.capacity
        && header.count >= 0 && header.count <= header.capacity;
}

inline std::streamoff SlotOffset(int index) {
    return static_cast<std::streamoff>(sizeof(QueueHeader))
        + static_cast<std::streamoff>(index) * static_cast<std::streamoff>(sizeof(MessageSlot));
}