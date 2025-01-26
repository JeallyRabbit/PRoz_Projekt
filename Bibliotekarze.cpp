#include <mpi.h>
#include <iostream>
#include <vector>
#include <queue>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <algorithm>

#define TAG_REQUEST 1
#define TAG_APPROVE 2
#define TAG_RESERVE 3
#define TAG_RELEASE 4
#define TAG_REJECT 5
#define TAG_ACK 6

struct MPCStatus {
    int mpc_id;
    int reserved_by; // -1 oznacza, że jest wolny
};

struct Message {
    int lamport_time;
    int sender_rank;
    int mpc_id;
    int tag;

    bool operator<(const Message& other) const {
        if (lamport_time == other.lamport_time) {
            return sender_rank > other.sender_rank; // Niższy ranking ma wyższy priorytet - jest ważniejszy
        }
        return lamport_time > other.lamport_time; // Niższy zegar lamporta jest ma priiorytet - jest ważniejszy
    }
};

int lamport_time = 0;
int global_size = 0; // Globalna zmienna - ilość procesów na których przetwarzamy
std::priority_queue<Message> request_queue;

// Wypisywanie obecnego statusu MPC
void log_mpc_status(const std::vector<MPCStatus>& mpc_status, int rank) {
    std::cout << "Proces " << rank << ": Stan MPCs: [";
    for (const auto& mpc : mpc_status) {
        std::cout << "{mpc_id: " << mpc.mpc_id << ", reserved_by: " << mpc.reserved_by << "}, ";
    }
    std::cout << "]" << std::endl;
}

// Wyślij wiadomość z czasem lamporta
void send_message_with_time(int target, int tag, int mpc_id, int rank) {
    lamport_time++;
    int message_data[4] = {lamport_time, rank, mpc_id, tag};
    MPI_Send(message_data, 4, MPI_INT, target, tag, MPI_COMM_WORLD);
}

// Zaktualizuj czas lamporta (wzór z algorytmu)
void update_lamport_time(int received_time) {
    lamport_time = std::max(lamport_time, received_time) + 1;
}

// wyślij wiadomość (nie blokująca działania procesu o zajeciu MPC)
void post_async_receive(MPI_Request* request, int* buffer) {
    MPI_Irecv(buffer, 4, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, request);
}

// Przetwarzanie otrzymanej wiadomości - rózne tagi oznaczaja inna treść wiadomości
void process_message(const Message& msg, std::vector<MPCStatus>& mpc_status, int rank, int& mpc_to_request, int& approvals, int& conflicts) {
    update_lamport_time(msg.lamport_time);

    if (msg.tag == TAG_APPROVE) {
        approvals++;
    } else if (msg.tag == TAG_REJECT) {
        conflicts++;
    } else if (msg.tag == TAG_REQUEST) {
        request_queue.push(msg);
        if (mpc_status[msg.mpc_id].reserved_by == -1 ||  // Jeśli MPC jets wolny
            (mpc_status[msg.mpc_id].reserved_by == rank &&  // albo wybrany MPC jest wolny
             (msg.lamport_time > lamport_time || (msg.lamport_time == lamport_time && msg.sender_rank > rank)))) {
            send_message_with_time(msg.sender_rank, TAG_APPROVE, msg.mpc_id, rank);
        }
    } else if (msg.tag == TAG_RESERVE) {
        mpc_status[msg.mpc_id] = {msg.mpc_id, msg.sender_rank};
        std::cout << "Proces " << rank << ": Otrzymano informację o rezerwacji MPC " << msg.mpc_id
                  << " przez proces " << msg.sender_rank << " (czas: " << lamport_time << ")." << std::endl;
        log_mpc_status(mpc_status, rank);

        // Sprawdź czy rezerwowany (przez inny proces) MPC jets tym o który proces przetwarzający wiadomość się ubiega - czy chodzi o "mój" MPC
        if (msg.mpc_id == mpc_to_request) {
            std::cout << "Proces " << rank << ": Wybrany MPC " << mpc_to_request << " został zarezerwowany przez inny proces. Szukam nowego MPC." << std::endl;
            mpc_to_request = -1;

            // Szukaj innego wolnego MPC
            for (int i = 0; i < mpc_status.size(); ++i) {
                if (mpc_status[i].reserved_by == -1) {
                    mpc_to_request = i;
                    break;
                }
            }

            if (mpc_to_request != -1) {
                std::cout << "Proces " << rank << ": Wybieram nowy MPC " << mpc_to_request << "." << std::endl;
                for (int i = 0; i < global_size; ++i) {
                    if (i != rank) {
                        send_message_with_time(i, TAG_REQUEST, mpc_to_request, rank);
                    }
                }
            } else {
                std::cout << "Proces " << rank << ": Brak dostępnych MPC. Czekam." << std::endl;
            }
        }

        send_message_with_time(msg.sender_rank, TAG_ACK, msg.mpc_id, rank);
    } else if (msg.tag == TAG_RELEASE) {
        mpc_status[msg.mpc_id] = {msg.mpc_id, -1};
        std::cout << "Proces " << rank << ": Otrzymano informację o zwolnieniu MPC " << msg.mpc_id
                  << " (czas: " << lamport_time << ")." << std::endl;
        log_mpc_status(mpc_status, rank);
        send_message_with_time(msg.sender_rank, TAG_ACK, msg.mpc_id, rank);
    } else if (msg.tag == TAG_ACK) {
        approvals++;
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &global_size); // Inicjalizacja globalnego rozmiaru (liczby procesów)

    const int M = 2; // Liczba MPC
    std::vector<MPCStatus> mpc_status;
    for (int i = 0; i < M; ++i) {
        mpc_status.push_back({i, -1}); // Inicjalizacja MPC z ich ID i stanem "niezarezerwowany" 
    }

    MPI_Status status;
    srand(time(nullptr) + rank);

    int buffer[4];
    MPI_Request request;
    post_async_receive(&request, buffer);

    while (true) {
        sleep(rand() % 4 + 1); // Zasymulowanie opóźnienia pomiędzy żądaniami

        // Przetwarzanie otrzymanych wiadomości - asynchronicznie -> proces zajmuje się swoją "pracą" np. szukaniem MPC czy poganianiem czytelników i w tym samym czasie przetwarza wiadomości od innych
        while (!request_queue.empty()) {
            Message msg = request_queue.top();
            request_queue.pop();
            int temp_approvals = 0;
            int temp_conflicts = 0;
            process_message(msg, mpc_status, rank, msg.mpc_id  , temp_approvals, temp_conflicts);
            //process_message(msg, mpc_status, rank, mpc_to_request, approvals, conflicts);
        }

        // Znajdź pierwszego wolnego MPC
        int mpc_to_request = -1;
        for (int i = 0; i < M; ++i) {
            if (mpc_status[i].reserved_by == -1) {
                mpc_to_request = i;
                break;
            }
        }

        if (mpc_to_request == -1) {
            std::cout << "Proces " << rank << ": Wszystkie MPC są zajęte, czekam."
                      << std::endl;
            log_mpc_status(mpc_status, rank);
            continue;
        }

        std::cout << "Proces " << rank << ": Żądam rezerwacji MPC " << mpc_to_request << " (czas: " << lamport_time << ")." << std::endl;

        // Żądanie rezerwacji
        for (int i = 0; i < global_size; ++i) { 
            if (i != rank) {
                send_message_with_time(i, TAG_REQUEST, mpc_to_request, rank);
            }
        }

        // Czekanie na odpowiedźi od wszystkich innych procesów - akceptacje lub odmowy (konflikt)
        int approvals = 0, conflicts = 0;
        while (approvals + conflicts < global_size - 1) {
            int flag;
            MPI_Test(&request, &flag, &status);
            if (flag) {
                // Przetwarzanie otrzymanych wiadomości
                Message msg = {buffer[0], buffer[1], buffer[2], buffer[3]};
                process_message(msg, mpc_status, rank, mpc_to_request, approvals, conflicts);
                post_async_receive(&request, buffer);
            }
        }

        if (conflicts > 0) {
            std::cout << "Proces " << rank << ": Konflikt w rezerwacji MPC " << mpc_to_request << " (czas: " << lamport_time << "), anuluję próbę." << std::endl;
            continue;
        }

        // Przetworzenie rezerwacji - informowanie innych o tym, że proces rezerwuje MPC
        for (int i = 0; i < global_size; ++i) {
            if (i != rank) {
                send_message_with_time(i, TAG_RESERVE, mpc_to_request, rank);
            }
        }

        // Czekanie na potwierdzenie od innych otrzymania wiadomości o zarezerwowaniu
        int reserve_acks = 0;
        while (reserve_acks < global_size - 1) {
            int flag;
            MPI_Test(&request, &flag, &status);
            if (flag) {
                Message msg = {buffer[0], buffer[1], buffer[2], buffer[3]};
                if (msg.tag == TAG_ACK) {
                    reserve_acks++;
                } else {
                    process_message(msg, mpc_status, rank, mpc_to_request, approvals, conflicts);
                }
                post_async_receive(&request, buffer);
            }
        }

        mpc_status[mpc_to_request] = {mpc_to_request, rank};
        std::cout << "Proces " << rank << ": Zarezerwowałem MPC " << mpc_to_request << " (czas: " << lamport_time << ")." << std::endl;
        log_mpc_status(mpc_status, rank);

        //Losowy czas korzystania z MPC
        sleep(rand() % 3 + 1);


        // Informowanie innych o zwolnieniu MPC
        mpc_status[mpc_to_request] = {mpc_to_request, -1};
        for (int i = 0; i < global_size; ++i) {
            if (i != rank) {
                send_message_with_time(i, TAG_RELEASE, mpc_to_request, rank);
            }
        }

        // Czekanie na otryzmanie potwierdzeń o otrzymaniu wiadomości o zwolnieniu
        int release_acks = 0;
        while (release_acks < global_size - 1) {
            int flag;
            MPI_Test(&request, &flag, &status);
            if (flag) {
                Message msg = {buffer[0], buffer[1], buffer[2], buffer[3]};
                if (msg.tag == TAG_ACK) {
                    release_acks++;
                } else {
                    process_message(msg, mpc_status, rank, mpc_to_request, approvals, conflicts);
                }
                post_async_receive(&request, buffer);
            }
        }

        log_mpc_status(mpc_status, rank);
    }

    MPI_Finalize();
    return 0;
}

