#include "Common.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <vector>

namespace {

    const std::string SENDER_EXE_NAME = "OS_lab_4_2.exe";
    const char* const ENV_VARIABLE_NAME = "SKRYPKO";

    bool TryGetFileNameFromEnvironment(std::string& filename) {
        char* rawBuffer = nullptr;
        size_t bufferSize = 0;

        const errno_t err = _dupenv_s(&rawBuffer, &bufferSize, ENV_VARIABLE_NAME);
        std::unique_ptr<char, decltype(&std::free)> envBuffer(rawBuffer, &std::free);

        if (err != 0 || !envBuffer || envBuffer.get()[0] == '\0') {
            return false;
        }

        filename = Trim(envBuffer.get());
        return !filename.empty();
    }

    std::string ReadFileNameFromConsole() {
        while (true) {
            const std::string filename = Trim(ReadLine("Введите имя бинарного файла: "));
            if (!filename.empty()) {
                return filename;
            }
            std::cout << "[Ошибка] Имя файла не может быть пустым.\n";
        }
    }

    std::string GetBinaryFileName() {
        while (true) {
            std::cout << "Выберите способ получения имени бинарного файла:\n"
                << "1. Ввести с консоли\n"
                << "2. Получить из переменной среды (" << ENV_VARIABLE_NAME << ")\n";
            const int choice = ReadIntSafe("Ваш выбор (1 или 2): ", 1, 2);

            if (choice == 1) {
                return ReadFileNameFromConsole();
            }

            std::string filename;
            if (TryGetFileNameFromEnvironment(filename)) {
                std::cout << "[INFO] Имя файла из переменной среды: " << filename << "\n";
                return filename;
            }
            std::cout << "[Ошибка] Переменная среды '" << ENV_VARIABLE_NAME
                << "' не найдена или пуста. Выберите другой способ.\n\n";
        }
    }

    void InitializeBinaryFile(const std::string& filename, int recordCount) {
        std::ofstream out(filename, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error("Не удалось создать бинарный файл: " + filename);
        }

        const QueueHeader header{ 0, 0, recordCount, 0 };
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));

        const MessageSlot emptySlot{};
        for (int i = 0; i < recordCount; ++i) {
            out.write(reinterpret_cast<const char*>(&emptySlot), sizeof(emptySlot));
        }

        out.flush();
        if (!out.good()) {
            throw std::runtime_error("Ошибка записи в файл: " + filename);
        }
    }

    std::string ReadMessageFromQueue(const std::string& filename) {
        std::fstream file(filename, std::ios::binary | std::ios::in | std::ios::out);
        if (!file.is_open()) {
            throw std::runtime_error("Не удалось открыть файл: " + filename);
        }

        QueueHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file.good()) {
            throw std::runtime_error("Ошибка чтения заголовка.");
        }
        if (!IsHeaderValid(header)) {
            throw std::runtime_error("Заголовок файла поврежден.");
        }
        if (header.count == 0) {
            throw std::logic_error("Очередь пуста.");
        }

        file.seekg(SlotOffset(header.head), std::ios::beg);

        MessageSlot slot{};
        file.read(reinterpret_cast<char*>(&slot), sizeof(slot));
        if (!file.good()) {
            throw std::runtime_error("Ошибка чтения сообщения.");
        }
        slot.text[sizeof(slot.text) - 1] = '\0';

        header.head = (header.head + 1) % header.capacity;
        --header.count;

        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.flush();
        if (!file.good()) {
            throw std::runtime_error("Ошибка обновления заголовка.");
        }

        return std::string(slot.text);
    }

    void CheckCreated(const UniqueHandle& handle, DWORD error, const char* what) {
        if (!handle) {
            throw std::runtime_error(std::string("Ошибка ") + what + ": " + GetLastErrorAsString(error));
        }
        if (error == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error(std::string("Объект синхронизации уже существует (") + what
                + "). Закройте окна процессов предыдущего запуска и повторите попытку.");
        }
    }

    UniqueHandle CreateQueueMutex(const std::string& name) {
        UniqueHandle handle(CreateMutexA(nullptr, FALSE, name.c_str()));
        CheckCreated(handle, GetLastError(), "CreateMutex");
        return handle;
    }

    UniqueHandle CreateQueueSemaphore(const std::string& name, LONG initialCount, LONG maximumCount) {
        UniqueHandle handle(CreateSemaphoreA(nullptr, initialCount, maximumCount, name.c_str()));
        CheckCreated(handle, GetLastError(), "CreateSemaphore");
        return handle;
    }

    UniqueHandle CreateReadyEvent(const std::string& name) {
        UniqueHandle handle(CreateEventA(nullptr, TRUE, FALSE, name.c_str()));
        CheckCreated(handle, GetLastError(), "CreateEvent");
        return handle;
    }

    UniqueHandle LaunchSender(const std::string& filename, int senderId, DWORD receiverPid) {
        const std::string commandLine = "\"" + SENDER_EXE_NAME + "\" \"" + filename + "\" "
            + std::to_string(senderId) + " " + std::to_string(receiverPid);
        std::vector<char> buffer(commandLine.begin(), commandLine.end());
        buffer.push_back('\0');

        STARTUPINFOA startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo{};

        if (!CreateProcessA(nullptr, buffer.data(), nullptr, nullptr, FALSE,
            CREATE_NEW_CONSOLE, nullptr, nullptr, &startupInfo, &processInfo)) {
            const std::string reason = GetLastErrorAsString();
            throw std::runtime_error("Не удалось запустить " + SENDER_EXE_NAME + ": " + reason);
        }

        CloseHandle(processInfo.hThread);
        return UniqueHandle(processInfo.hProcess);
    }

    void WaitForSendersReady(const std::vector<HANDLE>& readyEvents, const std::vector<UniqueHandle>& senderProcesses) {
        while (true) {
            const DWORD result = WaitForMultipleObjects(
                static_cast<DWORD>(readyEvents.size()), readyEvents.data(), TRUE, 200);

            if (result == WAIT_OBJECT_0) {
                return;
            }
            if (result != WAIT_TIMEOUT) {
                throw std::runtime_error("Ошибка WaitForMultipleObjects: " + GetLastErrorAsString());
            }

            for (size_t i = 0; i < senderProcesses.size(); ++i) {
                if (IsProcessFinished(senderProcesses[i].get())
                    && WaitForSingleObject(readyEvents[i], 0) != WAIT_OBJECT_0) {
                    throw std::runtime_error("Процесс Sender #" + std::to_string(i)
                        + " завершился, не сообщив о готовности.");
                }
            }
        }
    }

    bool WaitForMessage(HANDLE hSemFilled, std::vector<HANDLE>& aliveSenders) {
        DWORD result = WaitForSingleObject(hSemFilled, 0);
        if (result == WAIT_OBJECT_0) {
            return true;
        }
        if (result != WAIT_TIMEOUT) {
            throw std::runtime_error("Ошибка семафора Filled: " + GetLastErrorAsString());
        }

        bool waitingAnnounced = false;
        while (true) {
            aliveSenders.erase(
                std::remove_if(aliveSenders.begin(), aliveSenders.end(), IsProcessFinished),
                aliveSenders.end());

            if (aliveSenders.empty()) {
                return WaitForSingleObject(hSemFilled, 0) == WAIT_OBJECT_0;
            }

            if (!waitingAnnounced) {
                std::cout << "[ОЖИДАНИЕ] Очередь пуста, ожидание нового сообщения...\n";
                waitingAnnounced = true;
            }

            std::vector<HANDLE> waitSet;
            waitSet.reserve(aliveSenders.size() + 1);
            waitSet.push_back(hSemFilled);
            waitSet.insert(waitSet.end(), aliveSenders.begin(), aliveSenders.end());

            result = WaitForMultipleObjects(
                static_cast<DWORD>(waitSet.size()), waitSet.data(), FALSE, INFINITE);

            if (result == WAIT_OBJECT_0) {
                return true;
            }
            if (result >= WAIT_OBJECT_0 + waitSet.size()) {
                throw std::runtime_error("Ошибка ожидания сообщения: " + GetLastErrorAsString());
            }
        }
    }

    void TerminateSenders(const std::vector<UniqueHandle>& senderProcesses) {
        for (const UniqueHandle& process : senderProcesses) {
            if (process && !IsProcessFinished(process.get())) {
                TerminateProcess(process.get(), 1);
            }
        }
    }

}

int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    std::vector<UniqueHandle> senderProcesses;

    try {
        std::cout << "=== ПРОЦЕСС RECEIVER ===\n";

        const std::string filename = GetBinaryFileName();
        const int recordCount = ReadIntSafe(
            "Введите количество записей (1 - " + std::to_string(MAX_RECORDS) + "): ", 1, MAX_RECORDS);

        InitializeBinaryFile(filename, recordCount);
        std::cout << "[INFO] Бинарный файл создан: " << filename << "\n";

        const int senderCount = ReadIntSafe(
            "Введите количество процессов Sender (1 - " + std::to_string(MAX_SENDERS) + "): ", 1, MAX_SENDERS);

        const DWORD receiverPid = GetCurrentProcessId();

        UniqueHandle hMutex = CreateQueueMutex(MakeObjectName(MUTEX_NAME, receiverPid));
        UniqueHandle hSemEmpty = CreateQueueSemaphore(
            MakeObjectName(SEM_EMPTY_NAME, receiverPid), recordCount, recordCount);
        UniqueHandle hSemFilled = CreateQueueSemaphore(
            MakeObjectName(SEM_FILLED_NAME, receiverPid), 0, recordCount);

        std::vector<UniqueHandle> readyEvents;
        std::vector<HANDLE> rawEventHandles;
        readyEvents.reserve(static_cast<size_t>(senderCount));
        rawEventHandles.reserve(static_cast<size_t>(senderCount));
        senderProcesses.reserve(static_cast<size_t>(senderCount));

        for (int i = 0; i < senderCount; ++i) {
            UniqueHandle hEvent = CreateReadyEvent(
                MakeObjectName(EVENT_READY_PREFIX + std::to_string(i), receiverPid));
            rawEventHandles.push_back(hEvent.get());
            readyEvents.push_back(std::move(hEvent));

            senderProcesses.push_back(LaunchSender(filename, i, receiverPid));
        }

        std::cout << "Ожидание готовности Sender...\n";
        WaitForSendersReady(rawEventHandles, senderProcesses);
        std::cout << "Все Sender готовы!\n";

        std::vector<HANDLE> aliveSenders;
        aliveSenders.reserve(senderProcesses.size());
        for (const UniqueHandle& process : senderProcesses) {
            aliveSenders.push_back(process.get());
        }

        while (true) {
            std::cout << "\n1. Прочитать сообщение\n2. Выход\n";
            const int action = ReadIntSafe("Выбор: ", 1, 2);

            if (action == 2) {
                break;
            }

            if (!WaitForMessage(hSemFilled.get(), aliveSenders)) {
                std::cout << "[INFO] Очередь пуста, а все процессы Sender уже завершили работу: "
                    << "новых сообщений не будет.\n";
                continue;
            }

            std::string message;
            {
                MutexLock lock(hMutex.get());
                message = ReadMessageFromQueue(filename);
            }

            if (!ReleaseSemaphore(hSemEmpty.get(), 1, nullptr)) {
                const std::string reason = GetLastErrorAsString();
                throw std::runtime_error("Ошибка ReleaseSemaphore (Empty): " + reason);
            }

            std::cout << "Получено: " << message << "\n";
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "Ошибка: " << ex.what() << "\n";
        TerminateSenders(senderProcesses);
        system("pause");
        return 1;
    }

    return 0;
}