#include <mpi.h>
#include <iostream>
#include <vector>
#include <queue>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <chrono>

#define TAG_REQUEST 1
#define TAG_APPROVE 2
#define TAG_RESERVE 3
#define TAG_RELEASE 4
#define TAG_ACK 5

struct MPCStatus {
    int mpc_id;
    int reserved_by; // -1 znaczy wolny
};

struct Message {
    int lamport_time;
    int sender_rank;
    int mpc_id;
    int tag;

    bool operator<(const Message& other) const {
        if (lamport_time == other.lamport_time) {
            return sender_rank > other.sender_rank; // Mniejszy ranking ma większy priorytet
        }
        return lamport_time > other.lamport_time; // Mniejszy czas Lamporta ma większy priorytet
    }
};

int lamport_time = 0;
int global_size = 0; // Liczba procesów
std::priority_queue<Message> message_queue; // Kolejka przychodzących wiadomości
std::mutex queue_mutex;
std::condition_variable queue_cv;

// Funkcja wypisywania stanów MPC
void log_mpc_status(const std::vector<MPCStatus>& mpc_status, int rank) {
    std::cout << "Proces " << rank << ": Stan MPCs: [";
    for (const auto& mpc : mpc_status) {
        std::cout << "{mpc_id: " << mpc.mpc_id << ", zarezerwowany przez: " << mpc.reserved_by << "}, ";
    }
    std::cout << "]" << std::endl;
}

// Funkcja losowego czekania
void random_wait(int a) {
    int wait_time = rand() % a; // Losowy czas w milisekundach (do 500 ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_time));
}

// Wyślij wiadomość z czasem Lamporta
void send_message_with_time(int target, int tag, int mpc_id, int rank) {
    lamport_time++;
    int message_data[4] = {lamport_time, rank, mpc_id, tag};
    MPI_Send(message_data, 4, MPI_INT, target, tag, MPI_COMM_WORLD);
}

// Zaktualizuj czas Lamporta
void update_lamport_time(int received_time) {
    lamport_time = std::max(lamport_time, received_time) + 1;
}

// Przetwarzanie - interpretowanie pojedynczej wiadomości
void process_message(const Message& msg, std::vector<MPCStatus>& mpc_status, int rank, int& mpc_to_request, int& approvals, int& conflicts) {
    if (msg.tag == TAG_APPROVE) {
        approvals++;
    } else if (msg.tag == TAG_REQUEST) {
        if (msg.mpc_id == mpc_to_request) {
            // Konflikt dotyczy tego samego MPC
            if ((msg.lamport_time < lamport_time) ||
                (msg.lamport_time == lamport_time && msg.sender_rank < rank)
                && mpc_status[mpc_to_request].reserved_by!=rank) {
                // Inny proces ma pierwszeństwo (mniejszy zegar lub mniejszy rank)
                std::cout << "Proces " << rank << ": Ustępuję procesowi " << msg.sender_rank
                          << " dla MPC " << msg.mpc_id << " (czas: " << lamport_time << ")." << std::endl;

                
                // Wyślij TAG_APPROVE do procesu, który ma pierwszeństwo - przyzwolenie na użycie MPC
                send_message_with_time(msg.sender_rank, TAG_APPROVE, msg.mpc_id, rank);

                // Wycofaj się z rezerwacji MPC
                mpc_to_request = -1;
            } else {
                // Nasz proces ma pierwszeństwo, ignorujemy
                std::cout << "Proces " << rank << ": Ignoruję żądanie procesu " << msg.sender_rank
                          << " dla MPC " << msg.mpc_id << " (czas: " << lamport_time << ")." << std::endl;
            }
        } else {
            // Żądanie dotyczy innego MPC, automatycznie zatwierdzamy
            send_message_with_time(msg.sender_rank, TAG_APPROVE, msg.mpc_id, rank);
            std::cout << "Proces " << rank << ": Zatwierdzam żądanie procesu " << msg.sender_rank
                      << " dla MPC " << msg.mpc_id << " (czas: " << lamport_time << ")." << std::endl;
        }
    } else if (msg.tag == TAG_RESERVE) {
        mpc_status[msg.mpc_id] = {msg.mpc_id, msg.sender_rank};
        std::cout << "Proces " << rank << ": Otrzymano informację o rezerwacji MPC " << msg.mpc_id
                  << " przez proces " << msg.sender_rank << " (czas: " << lamport_time << ")." << std::endl;
        log_mpc_status(mpc_status, rank);
    } else if (msg.tag == TAG_RELEASE) {
        mpc_status[msg.mpc_id] = {msg.mpc_id, -1}; // Aktualizacja tablicy stanów MPC - zaktualizowanie tablicy o informacje o zwolnieniu
        std::cout << "Proces " << rank << ": Otrzymano informację o zwolnieniu MPC " << msg.mpc_id
                  << " (czas: " << lamport_time << ")." << std::endl;
        log_mpc_status(mpc_status, rank);
    }

    update_lamport_time(msg.lamport_time);
}

// Obsługa wiadomości w wątku odbierającym
void message_handling_thread(std::vector<MPCStatus>& mpc_status, int rank, int& mpc_to_request, int& approvals, int& conflicts) {
    int buffer[4];
    MPI_Status status;
    while (true) {
        MPI_Recv(buffer, 4, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        Message msg = {buffer[0], buffer[1], buffer[2], buffer[3]};

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            process_message(msg, mpc_status, rank, mpc_to_request, approvals, conflicts);
        }
        queue_cv.notify_all(); // Powiadomienie głównego wątku o wiadomości
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &global_size);

    const int M = 2; // Liczba MPC
    std::vector<MPCStatus> mpc_status;
    for (int i = 0; i < M; ++i) {
        mpc_status.push_back({i, -1}); // Inicjalizacja MPC z ich ID i stanem "niezarezerwowany" 
    }

    int mpc_to_request = -1;
    int approvals = 0, conflicts = 0;

    // Startowanie wątku obsługującego wiadomości
    std::thread msg_thread(message_handling_thread, std::ref(mpc_status), rank, std::ref(mpc_to_request), std::ref(approvals), std::ref(conflicts));

    while (true) {

    

        // Znajdź wolnego MPC
        if (mpc_to_request == -1) {
            random_wait(100);
            for (int i = 0; i < M; ++i) {
                if (mpc_status[i].reserved_by == -1) {
                    mpc_to_request = i;
                    break;
                }

            }
        }

        // Nie znaleziono wolnych MPC - idź szukaj jeszcze raz
        if (mpc_to_request == -1) {
            continue;
        }

        std::cout << "Proces " << rank << ": Żądam rezerwacji MPC " << mpc_to_request << " (czas: " << lamport_time << ")." << std::endl;

        // Wysłanie żądań rezerwacji
        approvals = 0;
        conflicts = 0;
        for (int i = 0; i < global_size; ++i) {
            if (i != rank) {
                send_message_with_time(i, TAG_REQUEST, mpc_to_request, rank);
            }
        }

        // Czekanie na odpowiedzi
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [&]() { return approvals + conflicts >= global_size - 1; });
        }

        
        

        // jeśli znaleziono MPC to wykorzystania - gdyby proces oddał swojego wybranego MPC innemu procesowi to ustawi swoje mpc_to_request na -1
        if(mpc_to_request!=-1)
        {
            // Zarezerwowanie MPC - powiadomienie innych o rezerwacji 
            for (int i = 0; i < global_size; ++i) {
                if (i != rank) {
                    send_message_with_time(i, TAG_RESERVE, mpc_to_request, rank);
                }
            }

            mpc_status[mpc_to_request] = {mpc_to_request, rank};
            std::cout << "Proces " << rank << ": Zarezerwowałem MPC " << mpc_to_request << "." << std::endl;
            log_mpc_status(mpc_status, rank);

            // Symulacja używania MPC - używanie przez czas o 10 sekund
            random_wait(10000);

            // Zwolnienie MPC
            mpc_status[mpc_to_request] = {mpc_to_request, -1};
            for (int i = 0; i < global_size; ++i) {
                if (i != rank) {
                    send_message_with_time(i, TAG_RELEASE, mpc_to_request, rank);
                }
            }
            std::cout << "Proces " << rank << ": Zwolniłem MPC " << mpc_to_request << "." << std::endl;
            mpc_to_request = -1;
            log_mpc_status(mpc_status, rank);
        }
    }

    msg_thread.join();
    MPI_Finalize();
    return 0;
}
