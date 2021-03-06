#ifndef BFG_HASHTABLES_HPP
#define BFG_HASHTABLES_HPP

#include "google/sparse_hash_map"
#include "google/sparsehash/sparsehashtable.h"

#include "hash.hpp"
#include "KmerIntPair.hpp"


using google::sparse_hash_map;

typedef google::sparse_hashtable<KmerIntPair, Kmer, KmerHash, SelectKmerKey, SetKmerKey, std::equal_to<Kmer>, std::allocator<KmerIntPair> > hmap_t;

typedef google::sparse_hash_map<Kmer, float, KmerHash> hmapq_t;

#endif // BFG_HASHTABLES_HPP
