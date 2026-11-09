
// stl
#include <iostream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <fstream>
#include <sys/time.h>

using namespace std;

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
using std::string;

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

enum class LastCall { None, Send, Recv };

const static int NETWORK_BUFFER_SIZE =
    1024 * 1024; // Should change depending on the network


class NetIO {
public:
  bool is_server;
  int mysocket = -1;
  int consocket = -1;
  FILE *stream = nullptr;
  char *buffer = nullptr;
  bool has_sent = false;
  string addr;
  int port;
  uint64_t counter = 0;
  uint64_t num_rounds = 0;
  bool FBF_mode;
  LastCall last_call = LastCall::None;
  NetIO(const char *address, int port, bool full_buffer = false,
        bool quiet = false) {
    this->port = port;
    is_server = (address == nullptr);
    if (address == nullptr) {
      struct sockaddr_in dest;
      struct sockaddr_in serv;
      socklen_t socksize = sizeof(struct sockaddr_in);
      memset(&serv, 0, sizeof(serv));
      serv.sin_family = AF_INET;
      serv.sin_addr.s_addr =
          htonl(INADDR_ANY);       /* set our address to any interface */
      serv.sin_port = htons(port); /* set the server port number */
      mysocket = socket(AF_INET, SOCK_STREAM, 0);
      int reuse = 1;
      setsockopt(mysocket, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse,
                 sizeof(reuse));
      if (::bind(mysocket, (struct sockaddr *)&serv, sizeof(struct sockaddr)) <
          0) {
        perror("error: bind");
        exit(1);
      }
      if (listen(mysocket, 1) < 0) {
        perror("error: listen");
        exit(1);
      }
      consocket = accept(mysocket, (struct sockaddr *)&dest, &socksize);
      close(mysocket);
    } else {
      addr = string(address);

      struct sockaddr_in dest;
      memset(&dest, 0, sizeof(dest));
      dest.sin_family = AF_INET;
      dest.sin_addr.s_addr = inet_addr(address);
      dest.sin_port = htons(port);

      while (1) {
        consocket = socket(AF_INET, SOCK_STREAM, 0);

        if (connect(consocket, (struct sockaddr *)&dest,
                    sizeof(struct sockaddr)) == 0) {
          break;
        }

        close(consocket);
        usleep(1000);
      }
    }
    set_nodelay();
    stream = fdopen(consocket, "wb+");
    buffer = new char[NETWORK_BUFFER_SIZE];
    memset(buffer, 0, NETWORK_BUFFER_SIZE);
    if (full_buffer) {
      setvbuf(stream, buffer, _IOFBF, NETWORK_BUFFER_SIZE);
    } else {
      setvbuf(stream, buffer, _IONBF, NETWORK_BUFFER_SIZE);
    }
    this->FBF_mode = full_buffer;
    if (!quiet)
      std::cout << "connected\n";
  }

  void sync() {
    int tmp = 0;
    if (is_server) {
      send_data(&tmp, 1);
      recv_data(&tmp, 1);
    } else {
      recv_data(&tmp, 1);
      send_data(&tmp, 1);
      flush();
    }
  }

  ~NetIO() {
    fflush(stream);
    close(consocket);
    delete[] buffer;
  }

  void set_FBF() {
    flush();
    setvbuf(stream, buffer, _IOFBF, NETWORK_BUFFER_SIZE);
  }

  void set_NBF() {
    flush();
    setvbuf(stream, buffer, _IONBF, NETWORK_BUFFER_SIZE);
  }

  void set_nodelay() {
    const int one = 1;
    setsockopt(consocket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  }

  void set_delay() {
    const int zero = 0;
    setsockopt(consocket, IPPROTO_TCP, TCP_NODELAY, &zero, sizeof(zero));
  }

  void flush() { fflush(stream); }

  void send_data(const void *data, size_t len) {
    if (last_call != LastCall::Send) {
      num_rounds++;
      last_call = LastCall::Send;
    }
    counter += len;
    size_t sent = 0;
    while (sent < len) {
      size_t res = fwrite(sent + (char *)data, 1, len - sent, stream);
      if (res >= 0)
        sent += res;
      else
        fprintf(stderr, "error: net_send_data %ld\n", res);
    }
    has_sent = true;
  }

  void recv_data(void *data, size_t len) {
    if (last_call != LastCall::Recv) {
      num_rounds++;
      last_call = LastCall::Recv;
    }
    if (has_sent)
      fflush(stream);
    has_sent = false;
    size_t sent = 0;
    while (sent < len) {
      size_t res = fread(sent + (char *)data, 1, len - sent, stream);
      if (res >= 0)
        sent += res;
      else
        fprintf(stderr, "error: net_send_data %ld\n", res);
    }
  }
};


using namespace std;

int main(int argc, char** argv){
    char* ip = nullptr;
    bool is_client = false;
    if(!strcmp(argv[1], "-c")){
        is_client = true;
        ip = argv[2];
    }

    NetIO* io;
    // initiate socket connection
    if(is_client){
        io = new NetIO(ip, 8000, false, true);
        if (!io) {
            cerr << "Error: Failed to initialize NetIO for main connection." << endl;
            exit(EXIT_FAILURE);
        }
    } else {
        io = new NetIO(nullptr, 8000, false, true);
        if (!io) {
            cerr << "Error: Failed to initialize NetIO for main connection." << endl;
            exit(EXIT_FAILURE);
        }
    }

    cout << "Connection Established with " << (is_client ? ip : "client") << "\n";


    if(is_client){
        int rounds = std::stoi(argv[4]);

        long data_len = std::stol(argv[6]);    // no. of bytes

        long quantum = data_len/rounds;
        
        std::chrono::duration<double> total_duration;

        if(argc == 8 && !strcmp(argv[7], "-p")){
          long req = -2;
          unsigned char* data = new unsigned char[8000000000];

          io->send_data(&req, sizeof(long));
          for(int i = 0; i < 1; i++){
            //   cout << "i = " << i << "\n";
              io->recv_data(data, 8000000000*sizeof(char));
          }
          delete[] data;
        }

        cout << "--------------------------------------------------------------------\n";
        for(int r = 0; r < rounds; r++){
            auto st_round = std::chrono::high_resolution_clock::now();    
            
            io->send_data(&quantum, sizeof(long));

            unsigned char* data = new unsigned char[quantum];
            io->recv_data(data, quantum*sizeof(char));

            auto en_round = std::chrono::high_resolution_clock::now();    
            
            cout << "[" << r+1 << "] Received " << quantum*sizeof(char)*1.0/(1024*1024) << " MB; Time = " << (en_round - st_round).count()*1.0/(1e6) << " ms\n";

            total_duration += (en_round - st_round);
        }

        quantum = -1;
        io->send_data(&quantum, sizeof(long));
        io->recv_data(&quantum, sizeof(long));

        cout << "--------------------------------------------------------------------\n";
        cout << "Total data = " << quantum*1.0/(1024*1024) << " MB\n";
        cout << "Total time = " << total_duration.count()*1000 << " ms\n";
        cout << "Eff. Bandwidth = " << quantum*8.0/(1024*1024*1024*total_duration.count()) << " Gbit/s\n";

    } else {
        long comm = io->counter;
        long quantum = 0;

        while (true) {
            quantum = 0;
            io->recv_data(&quantum, sizeof(long));

            if(quantum == -2){
                // preamble
                const unsigned char* dummy_data = new unsigned char[8000000000]; 
                long comm = io->counter;
                for(int i = 0; i < 1; i++){
                    io->send_data(dummy_data, 8000000000 * sizeof(unsigned char));
                    cout << "\rPreamble chunk " << i+1 << " sent" << std::flush;
                }
                cout << "\nSent a preamble of size " << (io->counter - comm)*1.0/(1024*1024*1024) << " GB\n";

                io->counter = comm;
                continue;
            }

            if(quantum == -1){
                long total_data = io->counter - comm;
                io->send_data(&total_data, sizeof(long));

                cout << "Total Data Communicated = " << total_data*1.0/(1024*1024) << " MB \n"; 
                cout << "Closing server...\n";
                return 0;
            }

            unsigned char* data = new unsigned char[quantum];
            io->send_data(data, quantum*sizeof(char));
        }
    }

    return 0;
}