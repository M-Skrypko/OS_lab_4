#include "Common.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {

    void WriteMessageToQueue(const std::string& filename, const std::string& message) {
        if (message.empty() || message.size() >= sizeof(MessageSlot::text)) {
            throw std::length_error("Недопустимая длина сообщения.");
        }

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
        if (header.count >= header.capacity) {
            throw std::logic_error("Очередь переполнена.");
        }

        MessageSlot slot{};
        std::memcpy(slot.text, message.data(), message.size());

        file.seekp(SlotOffset(header.tail), std::ios::beg);
        file.write(reinterpret_cast<const char*>(&slot), sizeof(slot));

        header.tail = (header.tail + 1) % header.capacity;
        ++header.count;

        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.flush();

        if (!file.good()) {
            throw std::runtime_error("Ошибка записи.");
        }
    }

    std::string ReadMessage() {
        while (true) {
            const std::string message = Trim(ReadLine("Введите сообщение (< 20 символов): "));

            if (message.empty()) {
                std::cout << "[Ошибка] Пустое сообщение.\n";
            }
            else if (CountUtf8Chars(message) >= MAX_MESSAGE_LENGTH || message.size() >= MESSAGE_BUFFER_SIZE) {
                std::cout << "[Ошибка] Слишком длинное сообщение: допускается не более "
                    << (MAX_MESSAGE_LENGTH - 1) << " символов.\n";
            }
            else {
                return message;
            }
        }
    }

    void ReportReceiverFinished() {
        std::cout << "[INFO] Процесс Receiver завершил работу, отправка сообщений невозможна.\n";
        system("pause");
    }

    bool WaitForFreeSlot(HANDLE hSemEmpty, HANDLE hReceiver) {
        const HANDLE handles[2] = { hReceiver, hSemEmpty };

        DWORD result = WaitForMultipleObjects(2, handles, FALSE, 0);
        if (result == WAIT_TIMEOUT) {
            std::cout << "[ОЖИДАНИЕ] Очередь заполнена, ожидание свободного места...\n";
            result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        }

        if (result == WAIT_OBJECT_0 + 1) {
            return true;
        }
        if (result == WAIT_OBJECT_0) {
            return false;
        }
        throw std::runtime_error("Ошибка ожидания свободного места: " + GetLastErrorAsString());
    }

}

int main(int argc, char* argv[]) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    try {
        if (argc < 4) {
            throw std::invalid_argument(
                "Недостаточно аргументов. Ожидается: <имя файла> <номер Sender> <PID Receiver>.");
        }

        const std::string filename = argv[1];
        const int senderId = std::stoi(argv[2]);
        const DWORD receiverPid = static_cast<DWORD>(std::stoul(argv[3]));

        std::cout << "=== SENDER #" << senderId << " ===\n";

        {
            std::fstream testOpen(filename, std::ios::binary | std::ios::in | std::ios::out);
            if (!testOpen.is_open()) {
                throw std::runtime_error("Не удалось открыть файл: " + filename);
            }
        }

        UniqueHandle hReceiver(OpenProcess(SYNCHRONIZE, FALSE, receiverPid));
        if (!hReceiver) {
            throw std::runtime_error("Ошибка OpenProcess (Receiver): " + GetLastErrorAsString());
        }

        UniqueHandle hMutex(OpenMutexA(MUTEX_ALL_ACCESS, FALSE, MakeObjectName(MUTEX_NAME, receiverPid).c_str()));
        if (!hMutex) {
            throw std::runtime_error("Ошибка OpenMutex: " + GetLastErrorAsString());
        }

        UniqueHandle hSemEmpty(OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, MakeObjectName(SEM_EMPTY_NAME, receiverPid).c_str()));
        if (!hSemEmpty) {
            throw std::runtime_error("Ошибка OpenSemaphore (Empty): " + GetLastErrorAsString());
        }

        UniqueHandle hSemFilled(OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, MakeObjectName(SEM_FILLED_NAME, receiverPid).c_str()));
        if (!hSemFilled) {
            throw std::runtime_error("Ошибка OpenSemaphore (Filled): " + GetLastErrorAsString());
        }

        const std::string eventName = MakeObjectName(EVENT_READY_PREFIX + std::to_string(senderId), receiverPid);
        UniqueHandle hReadyEvent(OpenEventA(EVENT_MODIFY_STATE, FALSE, eventName.c_str()));
        if (!hReadyEvent) {
            throw std::runtime_error("Ошибка OpenEvent: " + GetLastErrorAsString());
        }

        if (!SetEvent(hReadyEvent.get())) {
            throw std::runtime_error("Ошибка SetEvent: " + GetLastErrorAsString());
        }

        while (true) {
            std::cout << "\n1. Отправить сообщение\n2. Выход\n";
            const int action = ReadIntSafe("Выбор: ", 1, 2);

            if (action == 2) {
                break;
            }

            if (IsProcessFinished(hReceiver.get())) {
                ReportReceiverFinished();
                break;
            }

            const std::string message = ReadMessage();

            if (!WaitForFreeSlot(hSemEmpty.get(), hReceiver.get())) {
                ReportReceiverFinished();
                break;
            }

            {
                MutexLock lock(hMutex.get());
                WriteMessageToQueue(filename, message);
            }

            if (!ReleaseSemaphore(hSemFilled.get(), 1, nullptr)) {
                const std::string reason = GetLastErrorAsString();
                throw std::runtime_error("Ошибка ReleaseSemaphore (Filled): " + reason);
            }

            std::cout << "Отправлено: " << message << "\n";
        }
    }
    catch (const std::exception& ex) {
        std::cerr << "Ошибка: " << ex.what() << "\n";
        system("pause");
        return 1;
    }

    return 0;
}