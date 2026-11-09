#include <string>

using namespace std;

std::string CONFIG_DIR = std::string(BISON_SOURCE_DIR_PATH) + "/config/";
std::string DATA_DIR = std::string(BISON_REPOSITORY_DIR) + "/data/prepared/bison/";
std::string GRAPHS_DIR = DATA_DIR + "graphs/";
std::string RESULTS_DIR = DATA_DIR + "results/";
std::string ORAM_DATA_DIR = DATA_DIR + "oram/";


// network
std::string SERVER_IP = "127.0.0.1";
int         SERVER_PORT = 8000;
int         SERVER_PARTY = 1;
std::string CLIENT_IP = "127.0.0.1";
int         CLIENT_PORT = 8000;
int         CLIENT_PARTY = 2;
