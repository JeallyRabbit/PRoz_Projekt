#include <mpi.h>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <thread>
#include <chrono>

#define FREE -1 // MPC jest wolny
#define REQUEST 0 // Typ wiadomości: żądanie zasobu
#define RELEASE 1 // Typ wiadomości: zwolnienie zasobu
#define ACK 2     // Typ wiadomości: akceptacja żądania

// Funkcja do losowego czasu oczekiwania
void random_sleep(int max_time) {
    std::this_thread::sleep_for(std::chrono::milliseconds(rand() % max_time));
}

void fixed_sleep(int time_ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(time_ms));
}

void print_mpc_status(int rank, const std::vector<int>& mpc, int lamport_clock) {
    std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") stan MPC: [";
    for (size_t i = 0; i < mpc.size(); ++i) {
        std::cout << mpc[i] << (i < mpc.size() - 1 ? ", " : "");
    }
    std::cout << "]\n";
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    srand(time(NULL) + rank);

    // Liczba MPC (M) i bibliotekarzy (B)
    const int M = 3; // Liczba MPC
    const int B = size; // Liczba procesów (bibliotekarzy)

    // Tablica stanów MPC (wszyscy mają tę samą)
    std::vector<int> mpc(M, FREE);

    int lamport_clock = 0;

    while (true) {
        random_sleep(1000); // Czekanie przez losowy czas
        
        bool resource_acquired = false;
        bool has_conflict = false;

        while (!resource_acquired) {
            // Krok 2: Szukanie dostępnego MPC
            int selected_mpc = -1;
            for (int i = 0; i < M; ++i) {
                if (mpc[i] == FREE) {
                    selected_mpc = i;
                    break;
                }
            }

            if (selected_mpc == -1) {
                std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << "): Brak dostępnych MPC, czekanie 2 sekundy.\n";
                fixed_sleep(2000); // Czekanie 2 sekundy
                continue;
            }

            lamport_clock++;
            std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << "): Próba zajęcia MPC " << selected_mpc << "\n";

            // Krok 3: Informowanie o chęci zajęcia zasobu
            for (int i = 0; i < size; ++i) {
                if (i != rank) {
                    MPI_Send(&lamport_clock, 1, MPI_INT, i, REQUEST, MPI_COMM_WORLD);
                    std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") wysyła żądanie do procesu " << i << " dla MPC " << selected_mpc << "\n";
                }
            }

            int accept_count = 0;
            has_conflict = false;

            while (accept_count < size - 1) {
                MPI_Status status;
                int received_clock;
                MPI_Recv(&received_clock, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                
                //lamport_clock = std::max(lamport_clock, received_clock)+ 1;

                std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << "): Otrzymano wiadomość od procesu " << status.MPI_SOURCE << " (rec_clock: " << received_clock << ", lamp_clock: " << lamport_clock-1 << ", tag: " << status.MPI_TAG << ")\n";

                if (status.MPI_TAG == REQUEST) {
                    if (received_clock < lamport_clock || (received_clock == lamport_clock && status.MPI_SOURCE < rank)) {
                        // Konflikt: proces o wyższym priorytecie kontynuuje, nasz ustępuje
                        MPI_Send(&lamport_clock, 1, MPI_INT, status.MPI_SOURCE, ACK, MPI_COMM_WORLD);
                        std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") ustępuje na rzecz procesu  ("<< received_clock<<"<"<<lamport_clock <<") "<< status.MPI_SOURCE << " dla MPC " << selected_mpc << "\n";
                        has_conflict = true;
                        break;
                    } else {
                        // Nasz proces kontynuuje, ignorując konflikt
                        MPI_Send(&lamport_clock, 1, MPI_INT, status.MPI_SOURCE, ACK, MPI_COMM_WORLD);
                        std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") odpowiada akceptacją dla procesu " << status.MPI_SOURCE << " dla MPC " << selected_mpc << "\n";
                        accept_count++;
                    }
                } else if (status.MPI_TAG == RELEASE) {
                    // Zwolnienie zasobu, aktualizacja tablicy MPC
                    mpc[status.MPI_TAG] = FREE;
                    std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << "): Otrzymano informację o zwolnieniu MPC " << status.MPI_TAG << " od procesu " << status.MPI_SOURCE << "\n";
                }

                lamport_clock = std::max(lamport_clock, received_clock)+ 1;
            }

            if (has_conflict) {
                std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << "): Konflikt wykryty, czeka na ponowną próbę zajęcia MPC.\n";
                fixed_sleep(2000); // Czekanie 2 sekundy przed ponownym szukaniem
                continue;
            }

            // Krok 4.1: Aktualizacja tablicy MPC
            mpc[selected_mpc] = rank;
            std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") zajmuje MPC " << selected_mpc << "\n";
            print_mpc_status(rank, mpc, lamport_clock);
            resource_acquired = true;

            // Użycie MPC przez losowy czas
            random_sleep(1000);

            // Krok 5: Zwalnianie MPC
            lamport_clock++;
            mpc[selected_mpc] = FREE;
            for (int i = 0; i < size; ++i) {
                if (i != rank) {
                    MPI_Send(&lamport_clock, 1, MPI_INT, i, RELEASE, MPI_COMM_WORLD);
                    std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") informuje proces " << i << " o zwolnieniu MPC " << selected_mpc << "\n";
                }
            }

            std::cout << "Proces " << rank << " (Zegar: " << lamport_clock << ") zwalnia MPC " << selected_mpc << "\n";
            print_mpc_status(rank, mpc, lamport_clock);
        }
    }

    MPI_Finalize();
    return 0;
}
