#include <mpi.h>
#include <iostream>
#include <vector>
#include <queue>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

// Typy wiadomości
#define REQUEST_MPC 1
#define REPLY_MPC 2
#define RELEASE_MPC 3
#define SERVICE_MPC 4

// Liczba MPC i ich stan
int total_mpcs;
std::vector<int> mpc_status; // 0 - wolny, 1 - zajęty, 2 - w serwisie

void broadcast(int tag, int mpc_id, int sender, int size) {
    for (int i = 0; i < size; i++) {
        if (i != sender) {
            int message[1] = {mpc_id};
            MPI_Send(&message, 1, MPI_INT, i, tag, MPI_COMM_WORLD);
        }
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (argc != 3) {
        if (rank == 0) {
            std::cerr << "Usage: mpirun -np <total_processes> <program> <total_mpcs> <K>" << std::endl;
        }
        MPI_Finalize();
        return 1;
    }

    total_mpcs = std::atoi(argv[1]);
    int K = std::atoi(argv[2]); // Limit czytelników przed serwisem

    // Stan MPC: 0 - wolny, 1 - zajęty, 2 - w serwisie
    mpc_status.resize(total_mpcs, 0);
    std::vector<int> mpc_usage(total_mpcs, 0);

    srand(time(0) + rank);

    while (true) {
        // Symulacja decyzji bibliotekarza
        int decision = rand() % 2; // 50% szans na próbę użycia MPC
        if (decision == 1) {
            // Żądanie użycia MPC
            int mpc_id = -1;
            for (int i = 0; i < total_mpcs; i++) {
                if (mpc_status[i] == 0) {
                    mpc_id = i;
                    break;
                }
            }

            if (mpc_id != -1) {
                // Zajęcie MPC i broadcast informacji
                mpc_status[mpc_id] = 1;
                broadcast(REQUEST_MPC, mpc_id, rank, size);

                // Symulacja obsługi czytelnika
                std::cout << "Bibliotekarz " << rank << " używa MPC " << mpc_id << std::endl;
                sleep(rand() % 5 + 1);

                // Aktualizacja użycia MPC
                mpc_usage[mpc_id]++;
                if (mpc_usage[mpc_id] >= K) {
                    // MPC idzie do serwisu
                    mpc_status[mpc_id] = 2;
                    broadcast(SERVICE_MPC, mpc_id, rank, size);
                    sleep(2); // Czas naprawy
                    mpc_status[mpc_id] = 0; // Powrót MPC do służby
                    mpc_usage[mpc_id] = 0;
                    broadcast(RELEASE_MPC, mpc_id, rank, size);
                    std::cout << "Bibliotekarz " << rank << " oddał MPC " << mpc_id << " po serwisie." << std::endl;
                } else {
                    // Zwolnienie MPC
                    mpc_status[mpc_id] = 0;
                    broadcast(RELEASE_MPC, mpc_id, rank, size);
                    std::cout << "Bibliotekarz " << rank << " zwolnił MPC " << mpc_id << std::endl;
                }
            } else {
                std::cout << "Bibliotekarz " << rank << " nie znalazł dostępnego MPC." << std::endl;
            }
        }

        // Sprawdzanie wiadomości
        MPI_Status status;
        int message[1];
        while (MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status)) {
            MPI_Recv(&message, 1, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
            int mpc_id = message[0];
            switch (status.MPI_TAG) {
                case REQUEST_MPC:
                    mpc_status[mpc_id] = 1;
                    break;
                case RELEASE_MPC:
                    mpc_status[mpc_id] = 0;
                    break;
                case SERVICE_MPC:
                    mpc_status[mpc_id] = 2;
                    break;
            }
        }

        sleep(1); // Krótka przerwa między operacjami
    }

    MPI_Finalize();
    return 0;
}
