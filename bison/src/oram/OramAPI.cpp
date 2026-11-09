#include "OramAPI.h"

template<typename T, typename LabelT>
int DiskANNNode<T, LabelT>::dim = -1;

template<typename T, typename LabelT>
int DiskANNNode<T, LabelT>::n_neighbors = -1;

template<>
int DiskANNNode<float, node_id_t>::dim = -1;

template<>
int DiskANNNode<float, node_id_t>::n_neighbors = -1;


template<>
int DiskANNNode<int, node_id_t>::dim = -1;

template<>
int DiskANNNode<int, node_id_t>::n_neighbors = -1;


template<>
int DiskANNNode<uint, node_id_t>::dim = -1;

template<>
int DiskANNNode<uint, node_id_t>::n_neighbors = -1;

template<>
int DiskANNNode<unsigned char, unsigned int>::dim = -1;

template<>
int DiskANNNode<unsigned char, unsigned int>::n_neighbors = -1;

template<>
int DiskANNNode<signed char, unsigned int>::dim = -1;

template<>
int DiskANNNode<signed char, unsigned int>::n_neighbors = -1;

template<>
int DiskANNNode<float, unsigned int>::dim = -1;

template<>
int DiskANNNode<float, unsigned int>::n_neighbors = -1;

template<>
int DiskANNNode<unsigned char, unsigned short>::dim = -1;

template<>
int DiskANNNode<unsigned char, unsigned short>::n_neighbors = -1;


template<>
int DiskANNNode<signed char, unsigned short>::dim = -1;

template<>
int DiskANNNode<signed char, unsigned short>::n_neighbors = -1;


template<>
int DiskANNNode<float, unsigned short>::dim = -1;

template<>
int DiskANNNode<float, unsigned short>::n_neighbors = -1;


template<>
int DiskANNNode<unsigned char, int>::dim = -1;

template<>
int DiskANNNode<unsigned char, int>::n_neighbors = -1;


template<>
int DiskANNNode<signed char, int>::dim = -1;

template<>
int DiskANNNode<signed char, int>::n_neighbors = -1;