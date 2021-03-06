#ifndef BFG_KMERMAPPER_HPP
#define BFG_KMERMAPPER_HPP

#include <vector>
#include <string>

#include "Common.hpp"
#include "Kmer.hpp"
#include "CompressedSequence.hpp"
#include "Contig.hpp"
#include "FindContig.hpp"


/* Short description: 
 *  - A ContigRef can be:
 *    1) An empty reference
 *    2) A reference to a contig
 *    3) A reference to position inside another ContigRef
 *  - A chain of references always ends at a contig
 *  */
class ContigRef {
public:
  ContigRef() : isContig(true) { ref.contig = NULL;}
  ContigRef(uint32_t id, int32_t pos) : isContig(false) {ref.idpos.id=id; ref.idpos.pos = pos;}
  bool isEmpty() { return (isContig && ref.contig == NULL);}
  
  union ContigRefUnion_t {
    struct ContigRefProper_t {
      uint32_t id; // maps to ContigArray, managed by Mapper class
      int32_t pos; // negative is reverse
    } idpos;
    Contig *contig; // not managed by ContigRef, but by KmerMapper
  } ref;
  bool isContig;
};


/* Short description: 
 *  - Map kmers in a contig to ContigRefs
 *  - Store which ContigRefs refer to Contigs
 *  - Get contig at the end of a ContigRef chain
 *  - Get a ContigRef that a kmer maps to
 *  - Join contigs while maintaining right references
 *  - Pretty print a reference map of a Contig
 *  */
class KmerMapper {
public:
	typedef KmerHashTable<ContigRef> hmap_contig_t;
  typedef hmap_contig_t::iterator iterator;
  typedef hmap_contig_t::const_iterator const_iterator;

  KmerMapper(size_t init = 1000, size_t s = Kmer::k) : map(init), stride(s) { contigs.reserve(init); }
  ~KmerMapper();

  size_t memory() const;

  size_t addContig(const string &s);
  size_t addContig(const char *s);
  void mapContig(uint32_t id, int32_t len, const char *s);
  

  ContigRef find(const Kmer km); // maybe change 

  size_t stride; // store every stride-th kmer  
  const size_t size() const { return map.size(); }
  const size_t contigCount() const { return contigs.size(); }

  ContigRef getContig(const size_t id) const;
  ContigRef getContig(const ContigRef ref) const;

  void printContig(const size_t id);
  void printContigs();
  int writeContigs(int count1, string contigfilename, string graphfilename);

  size_t joinTwoContigs(ContigRef a, ContigRef b, int a_direction, int b_direction);
  size_t joinAllContigs();
  pair<size_t, size_t> splitAllContigs(); 
  pair<pair<size_t, size_t>, size_t> splitAndJoinAllContigs(); 

  int checkContigForward(Contig* c, Kmer km, ContigRef &found);
private:
  ContigRef find_rep(ContigRef a) const;

  hmap_contig_t map;
  vector<ContigRef> contigs;
};

#endif // BFG_KMERMAPPER_H
