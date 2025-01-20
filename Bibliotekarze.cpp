#include <mpi.h>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <algorithm>

#define TAG_REQUEST 1
#define TAG_APPROVE 2
#define TAG_RESERVE 3
#define TAG_RELEASE 4
#define TAG_REJECT 5

struct MPCStatus {
    int mpc_id;
    int reserved_by; // -1 oznacza, że jest wolny
};

int lamport_time = 0;

// Funkcja aktualizująca zegar Lamporta
void update_lamport_time(int received_time) {
    lamport_time = std::max(lamport_time, received_time) + 1;
}

// Wysyłanie wiadomości z zegarem Lamporta
void send_message_with_time(int target, int tag, int mpc_id, int rank) {
    lamport_time++;
    int message_data[3] = {lamport_time, rank, mpc_id};
    MPI_Send(message_data, 3, MPI_INT, target, tag, MPI_COMM_WORLD);
}

// Odbieranie wiadomości z zegarem Lamporta
void receive_message_with_time(int* received_time, int* sender_rank, int* mpc_id, MPI_Status* status) {
    int message_data[3];
    MPI_Recv(message_data, 3, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, status);
    *received_time = message_data[0];
    *sender_rank = message_data[1];
    *mpc_id = message_data[2];
    update_lamport_time(*received_time);
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank); // Pobierz ID procesu
    MPI_Comm_size(MPI_COMM_WORLD, &size); // Pobierz liczbę procesów

    const int M = 2; // Liczba MPC
    std::vector<MPCStatus> mpc_status(M, {-1, -1}); // Tablica stanów MPC: początkowo wszystkie wolne

    MPI_Status status;
    srand(time(nullptr) + rank);

    while (true) {
        // Symulacja działania bibliotekarza
        sleep(rand() % 5 + 1); // Czekanie przed żądaniem

        // Znalezienie pierwszego wolnego MPC
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
            continue;
        }

        std::cout << "Proces " << rank << ": Żądam rezerwacji MPC " << mpc_to_request << " (czas: " << lamport_time << ")." << std::endl;

        // Wysyłanie żądania do wszystkich innych procesów
        int conflicts = 0;
        for (int i = 0; i < size; ++i) {
            if (i != rank) {
                send_message_with_time(i, TAG_REQUEST, mpc_to_request, rank);
            }
        }

        // Oczekiwanie na odpowiedzi od wszystkich innych procesów
        int approvals = 0;
        while (approvals + conflicts < size - 1) {
            int received_time, sender_rank, mpc_id;
            receive_message_with_time(&received_time, &sender_rank, &mpc_id, &status);

            if (status.MPI_TAG == TAG_APPROVE) {
                approvals++;
            } else if (status.MPI_TAG == TAG_REJECT) {
                conflicts++;
            } else if (status.MPI_TAG == TAG_REQUEST) {
                // Otrzymano żądanie od innego procesu
                if (mpc_id == mpc_to_request) {
                    // Konflikt o ten sam MPC
                    if (received_time < lamport_time || (received_time == lamport_time && sender_rank < rank)) {
                        // Inny proces ma priorytet
                        send_message_with_time(sender_rank, TAG_APPROVE, mpc_id, rank);
                    } else {
                        // Ten proces ma priorytet
                        send_message_with_time(sender_rank, TAG_REJECT, mpc_id, rank);
                    }
                } else {
                    // Żądanie o inny MPC, zatwierdzamy
                    send_message_with_time(sender_rank, TAG_APPROVE, mpc_id, rank);
                }
            } else if (status.MPI_TAG == TAG_RESERVE) {
                // Otrzymano informację o rezerwacji MPC
                mpc_status[mpc_id] = {mpc_id, sender_rank};
                std::cout << "Proces " << rank << ": Otrzymano informację o rezerwacji MPC " << mpc_id
                          << " przez proces " << sender_rank << " (czas: " << lamport_time << ")." << std::endl;
            } else if (status.MPI_TAG == TAG_RELEASE) {
                // Otrzymano informację o zwolnieniu MPC
                mpc_status[mpc_id] = {mpc_id, -1};
                std::cout << "Proces " << rank << ": Otrzymano informację o zwolnieniu MPC " << mpc_id
                          << " (czas: " << lamport_time << ")." << std::endl;
            }
        }

        if (conflicts > 0) {
            std::cout << "Proces " << rank << ": Konflikt w rezerwacji MPC " << mpc_to_request << " (czas: " << lamport_time << "), anuluję próbę." << std::endl;
            continue;
        }

        // Aktualizacja lokalnego stanu po uzyskaniu zgody
        mpc_status[mpc_to_request] = {mpc_to_request, rank};
        std::cout << "Proces " << rank << ": Zarezerwowałem MPC " << mpc_to_request << " (czas: " << lamport_time << ")." << std::endl;

        // Informowanie o rezerwacji
        for (int i = 0; i < size; ++i) {
            if (i != rank) {
                send_message_with_time(i, TAG_RESERVE, mpc_to_request, rank);
            }
        }

        // Symulacja korzystania z MPC
        sleep(rand() % 3 + 1);

        // Zwolnienie MPC
        mpc_status[mpc_to_request] = {mpc_to_request, -1};
        std::cout << "Proces " << rank << ": Zwolniłem MPC " << mpc_to_request << " (czas: " << lamport_time << ")." << std::endl;

        // Informowanie o zwolnieniu
        for (int i = 0; i < size; ++i) {
            if (i != rank) {
                send_message_with_time(i, TAG_RELEASE, mpc_to_request, rank);
            }
        }
    }

    MPI_Finalize();
    return 0;
}
