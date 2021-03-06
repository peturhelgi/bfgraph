#include "ContigMapper.hpp"
#include "CompressedSequence.hpp"
#include "KmerIterator.hpp"
#include <string>
#include <iterator>
#include <algorithm>
#include <fstream>


// for debugging
#include <iostream>

size_t stringMatch(const string& a, const string& b, size_t pos) {
  return distance(a.begin(),mismatch(a.begin(), a.end(), b.begin() + pos).first);
}

// use: delete cm
// pre:
// post: all memory allocated has been released
ContigMapper::~ContigMapper() {
  // we do not own bf pointer
  // long contigs could have pointers, but those should already be released
  hmap_long_contig_t::iterator it = lContigs.begin(),it_end=lContigs.end();
  for (; it != it_end; ++it) {
    delete it->second; // the contig pointer
  }
}

// use:  ContigMapper(sz)
// pre:  sz >= 0
// post: new contigmapper object
ContigMapper::ContigMapper(size_t init) :  bf(NULL) {
  limit = Kmer::k;
  stride = Kmer::k;
}




// user: i = cm.contigCount()
// pre:
// post: i is the number of contigs in the mapper
size_t ContigMapper::contigCount() const {
  return lContigs.size() + sContigs.size();
}

// use:  cm.mapBloomFilter(bf)
// pre:  bf != null
// post: uses the bloom filter bf to map reads
void ContigMapper::mapBloomFilter(const BlockedBloomFilter *bf) {
  this->bf = bf;
}

// use:  cm.mapRead(km,pos,cc)
// pre:  cc is a reference to a current contig in cm, km maps to cc
// post: the coverage information in cc has been updated
void ContigMapper::mapRead(const ContigMap& cc) {
  if (cc.isEmpty) { // nothing maps, move on
    return;
  } else if (cc.isShort) {
    // find short contig
    hmap_short_contig_t::iterator it = sContigs.find(cc.head);
    CompressedCoverage& cov = it->second; // reference to the info
    // increase coverage
    cov.cover(cc.dist, cc.dist + cc.len-1);
  } else {
    // find long contig
    hmap_long_contig_t::iterator it = lContigs.find(cc.head);
    Contig *cont = it->second;
    cont->cover(cc.dist, cc.dist+cc.len-1);
    //cout << cc.head.toString() << " : " << cc.dist << " - " << cc.dist + cc.len-1 << endl;
    //cout << cont->ccov.toString() << endl;
  }
}

// use: b = cm.addContig(km,read)
// pre:
// post: either contig string containsin has been added and b == true
//       or it was present and the coverage information was updated, b == false
//       NOT Threadsafe!
bool ContigMapper::addContig(Kmer km, const string& read, size_t pos, const string& seq) {
  // find the contig string to add
  string s;
  bool selfLoop = false;
  if (!seq.empty()) {
    s = seq;
  } else {
    findContigSequence(km,s,selfLoop);
  }
  size_t k = Kmer::k;
  bool found = false;
  if (selfLoop) {
    KmerIterator it(s.c_str()), it_end;
    // ok, check if any other k-mer is mapped
    for (; it != it_end; ++it) {
      ContigMap loopCC = find(it->first);
      if (!loopCC.isEmpty) {
        //cout << it->first.toString() << "matches, pos " << it->second << " to contig with " << loopCC.head.toString() << endl;
        //cout << "strand: " << loopCC.strand << ", dist = " << loopCC.dist << endl;
        // split the read up from pos up to the position of

        Contig *contig = lContigs.find(loopCC.head)->second;
        int loopSize = contig->seq.size() - k + 1;
        int fwMatch = stringMatch(s, read, pos); // how many k-mer to match from the start
        // position of the matching k-mer within the string
        int matchPos = (int) it->second;
        if (loopCC.strand) {
          int readStart = loopCC.dist - matchPos;
          if (readStart < 0) {
            readStart += loopSize;
            int matchSize = std::min(loopSize - readStart, fwMatch);
            contig->cover(readStart, readStart+matchSize -1);
            fwMatch -= matchSize;
            readStart = 0;
          }
          if (fwMatch > 0) {
            contig->cover(readStart, readStart + fwMatch - 1);
          }
        } else {
          int readStart = loopCC.dist - matchPos;
          if (readStart < 0) {
            readStart += loopSize;
            int matchSize = std::min(readStart,fwMatch);
            contig->cover(readStart - matchSize, loopSize-1);
            fwMatch -= matchSize;
            readStart = loopSize -1;
          }
          if (fwMatch > 0) {
            contig->cover(readStart - fwMatch + 1, readStart);
          }
        }
        return true;
      }
    }
  }
  //cout << "adding " << s << endl;
  //cout << "read   " << read << endl;
  //cout << "km = " << km.toString() << ", pos = " << pos << endl;

  // head is the front
  const char *c = s.c_str();
  size_t len = s.size();

  Kmer head = Kmer(c);

  ContigMap cc = this->find(head);
  found = found ||  !cc.isEmpty;

  if (!found) {

    // proper new contig
    if (s.size()-k+1 < limit && !selfLoop) {
      // create a short contig
      sContigs.insert(make_pair(head, CompressedCoverage(s.size()-k+1)));
    } else {
      lContigs.insert(make_pair(head, new Contig(c)));

      // insert shortcuts every stride k-mers
      for (size_t i = stride; i < len-k; i += stride) {
        shortcuts.insert(make_pair(Kmer(c+i),make_pair(head,i)));
      }
      // also insert shortcut for last k-mer
      shortcuts.insert(make_pair(Kmer(c+len-k),make_pair(head,len-k)));

    }
  } else {
    //cout << "no wait, found it already" << endl;
  }

  // map the read
  cc = findContig(km,read, pos);
  cc.selfLoop = selfLoop;
  mapRead(cc);
  return found;
}

// use:  b = cm.checkTip(tip)
// pre:  tip is the head of a tip
// post: true if there is a full alternative branch to the tip
bool ContigMapper::checkTip(Kmer tip) {
  size_t k = Kmer::k;
  // check forward
  for (size_t i = 0; i < 4; i++) {
    Kmer fw = tip.forwardBase(alpha[i]);
    ContigMap cc = find(fw);
    if (!cc.isEmpty ) {
      auto it = lContigs.find(cc.head);
      if (it != lContigs.end()) {
        for (size_t j = 0; j < 4; j++) {
          Kmer alt = fw.backwardBase(alpha[j]);
          if (alt != tip) {
            ContigMap cc_alt = find(alt);
            if (!cc_alt.isEmpty && cc_alt.size >= k && !cc_alt.isShort) {
              auto alt_it = lContigs.find(cc_alt.head);
              if (alt_it != lContigs.end() && alt_it->second->ccov.isFull()) {
                return true;
              }
            }
          }
        }
      }
    }
  }

  // check backward
  for (size_t i = 0; i < 4; i++) {
    Kmer bw = tip.backwardBase(alpha[i]);
    ContigMap cc = find(bw);
    if (!cc.isEmpty) {
      auto it = lContigs.find(cc.head);
      if (it != lContigs.end()) {
        for (size_t j = 0; j < 4; j++) {
          Kmer alt = bw.forwardBase(alpha[j]);
          if (alt != tip) {
            ContigMap cc_alt = find(alt);
            if (!cc_alt.isEmpty && cc_alt.size >= k && !cc_alt.isShort) {
              auto alt_it = lContigs.find(cc_alt.head);
              if (alt_it != lContigs.end() && alt_it->second->ccov.isFull()) {
                return true;
              }
            }
          }
        }
      }
    }
  }


  return false;
}


// use:  cm.findContigSequence(km, s, selfLoop)
// pre:  km is in the bloom filter
// post: s is the contig containing the kmer km
//       and the first k-mer in s is smaller (wrt. < operator)
//       than the last kmer
//       selfLoop is true of the contig is a loop or hairpin
void ContigMapper::findContigSequence(Kmer km, string& s, bool& selfLoop) {
  //cout << " s = " << s << endl;
  string fw_s;
  Kmer end = km;
  Kmer last = end;
  Kmer twin = km.twin();
  selfLoop = false;
  char c;
  size_t j = 0;
  size_t dummy;
  //cout << end.toString();
  while (fwBfStep(end,end,c,dummy)) {
    if (end == km) {
      //cout << "Got a self-loop in contig: " << fw_s << endl;
      //cout << km.toString() << " => " << end.toString() << endl;
      selfLoop = true;
      break;
    } else if (end == twin) {
      break;
    } else if (end == last.twin()) {
      break;
    }
    j++;
    fw_s.push_back(c);
    last = end;
    //cout << string(j,' ') << end.toString() << endl;
  }
  string bw_s;
  Kmer front = km;
  Kmer first = front;
  if (!selfLoop) {
    while (bwBfStep(front,front,c,dummy)) {
      if (front == km) {
        //cout << "Got a self-loop in contig: " << fw_s << endl;
        //cout << km.toString() << " => " << end.toString() << endl;
        selfLoop = true;
        break;
      } else if (front == twin) {
        //selfLoop = true; // hairpins are not selfloops
        break;
      } else if (front == first.twin()) {
        break;
      }
      bw_s.push_back(c);
      first = front;
    }
    reverse(bw_s.begin(), bw_s.end());
  }

  size_t k = Kmer::k;
  s.reserve(k + fw_s.size()+bw_s.size());
  s.append(bw_s);
  //cout << "bw_s = " << bw_s << endl;

  char tmp[Kmer::MAX_K];
  km.toString(tmp);
  //cout << "tmp = " << tmp << endl;
  s.append(tmp);
  //cout << "fw_s = " << fw_s << endl;

  s.append(fw_s);

  const char *t = s.c_str();
  Kmer head(t);
  //cout << "After append, s = " << s << endl;
  Kmer tail = Kmer(t+s.size()-k).twin();
  if (tail < head) { // reverse complement the string
    s = CompressedSequence(s).rev().toString();
  }
  //cout << "After reverse, s = " << s << endl;
}


// use:  cc = cm.findContig(km,s,pos)
// pre:  s[pos,pos+k-1] is the kmer km
// post: cc contains either the reference to the contig position
//       or empty if none found
ContigMap ContigMapper::findContig(Kmer km, const string& s, size_t pos) const {
  assert(bf != NULL);
  size_t k = Kmer::k;

  Kmer end = km;
  Kmer last = end;
  Kmer twin = km.twin();

  // need to check if we find it right away, need to treat this common case
  ContigMap cc;

  cc = this->find(end);
  if (!cc.isEmpty && !cc.isShort) {
    // ok, fetch the sequence
    const CompressedSequence& seq = lContigs.find(cc.head)->second->seq;
    size_t km_dist = cc.dist;
    size_t jlen = 0;

    if (cc.strand) {
      jlen = seq.jump(s.c_str(), pos, cc.dist, false) -k + 1;
    } else {
      jlen = seq.jump(s.c_str(), pos, cc.dist+k-1, true) -k + 1;
      km_dist -= (jlen-1);
    }

    return ContigMap(cc.head, km_dist, jlen, cc.size, cc.strand, cc.isShort);
  }

  char c;
  string fw_s;
  size_t fw_dist = 0;
  bool selfLoop = false;
  bool hairpin = false;
  // check <k steps ahead in fw direction
  size_t fw_deg;
  while (fw_dist < stride && fwBfStep(end, end, c, fw_deg)) {
    if (end == km) {
      selfLoop = true;
      break;
    } else if (end == twin) {
      hairpin = true;
      break;
    } else if (end == last.twin()) {
      hairpin = true;
      end = last; // back up
      break;
    }
    fw_s.push_back(c);
    ++fw_dist;
    last = end;
  }


  int len = 1 + stringMatch(fw_s, s, pos+k);

  string bw_s;
  size_t bw_dist = 0;
  size_t bw_deg;
  bool reverseHairpin = false;
  Kmer front = km;
  Kmer first = front;
  while (bw_dist < stride && bwBfStep(front,front,c,bw_deg)) {
    if (front == km) {
      selfLoop = true;
      break; // ok, we've reached around
    } else if (front == twin) {
      if (bw_dist == 0) {
        // back up a bit
        front = first;
      }
      break;
    } else if (front == first.twin()) {
      reverseHairpin = true;
      front = first; // back up
      break;
    }
    ++bw_dist;
    first = front;
  }




  cc = this->find(end);
  if (! cc.isEmpty) {
    size_t km_dist = cc.dist; // is 0 if we have reached the end
    if (!selfLoop) {
      if (cc.strand) {
        if (hairpin && cc.dist == 0) {
          // we walked backwards to the hairpin
          km_dist = 0;
          cc.strand = false;
        } else {
          km_dist -= fw_dist;
        }
      } else {
        km_dist += fw_dist - (len-1);
      }
    } else {
      assert(end == km);
      km_dist = 0;
    }

    ContigMap rcc(cc.head, km_dist, len, cc.size, cc.strand, cc.isShort);
    rcc.selfLoop = selfLoop;
    rcc.isIsolated = (fw_deg == 0 && bw_deg == 0 && len < k );

    return rcc;
  } else {
    cc = this->find(front);
    if (! cc.isEmpty) {
      size_t km_dist = cc.dist;
      if (cc.strand) {
        km_dist += bw_dist;
      } else {
        km_dist -= (bw_dist + len-1);
      }

      ContigMap rcc(cc.head, km_dist, len, cc.size, cc.strand, cc.isShort);
      rcc.selfLoop = selfLoop;
      rcc.isIsolated = (fw_deg == 0 && bw_deg == 0 && len < k);
      return rcc;
    }
  }

  if (bw_dist == k || fw_dist == k || selfLoop) {
    Kmer short_end = km;
    size_t fd = 0;
    size_t dummy;
    while (fd < k && fwBfStep(short_end,short_end,c, dummy)) {
      ++fd;
      cc = this->find(short_end);
      if (! cc.isEmpty) {
        const CompressedSequence& seq = lContigs.find(cc.head)->second->seq;
        size_t km_dist = cc.dist;
        size_t jlen = 0;

        if (cc.strand) {
          km_dist -= fd;
          jlen = seq.jump(s.c_str(), pos, km_dist, false) -k + 1;
          assert(jlen > 0);
        } else {
          km_dist += fd; // location of the start k-mer
          jlen = seq.jump(s.c_str(), pos, km_dist+k-1, true) -k + 1;
          // jlen is how much of the fw_s matches the contig
          assert(jlen > 0);
          km_dist -= (jlen-1);
        }
        return ContigMap(cc.head, km_dist, jlen, cc.size, cc.strand, cc.isShort);
      }
    }
  }

  // nothing found, how much can we skip ahead?
  ContigMap rcc(len);
  rcc.isIsolated = (fw_deg == 0 && bw_deg == 0 && len < k);
  return rcc;
}

// use:  b = cm.bwBfStep(km,front,c,deg)
// pre:  km is in the bloom filter
// post: b is true if km is inside a contig, in that
//       case end is the bw link and c is the nucleotide used for the link.
//       if b is false, front and c are not updated
//       if km is an isolated self link (e.g. 'AAA') i.e. end == km then returns false
//       deg is the backwards degree of the front
bool ContigMapper::bwBfStep(Kmer km, Kmer& front, char& c, size_t& deg) const {
  size_t i,j;
  size_t bw_count = 0;
  deg = 0;
  //size_t k = Kmer::k;

  // check bw direction
  j = -1;
  for (i = 0; i < 4; ++i) {
    Kmer bw_rep = front.backwardBase(alpha[i]).rep();
    if (bf->contains(bw_rep)) {
      j = i;
      ++bw_count;
      if (bw_count > 1) {
        break;
      }
    }
  }

  if (bw_count != 1) {
    deg = bw_count;
    return false;
  }

  // only one k-mer in the bw link
  deg = 1;
  Kmer bw = front.backwardBase(alpha[j]);
  size_t fw_count = 0;
  for (i = 0; i < 4; ++i) {
    Kmer fw_rep = bw.forwardBase(alpha[i]).rep();
    if (bf->contains(fw_rep)) {
      ++fw_count;
      if (fw_count > 1) {
        break;
      }
    }
  }

  assert(fw_count >= 1);
  if (fw_count != 1) {
    return false;
  }

  if (bw != km) {
    // exactly one k-mer in bw, character used is c
    front = bw;
    c = alpha[j];
    return true;
  } else {
    return false;
  }
}

// use:  b = cm.fwBfStep(km,end,c,deg)
// pre:  km is in the bloom filter
// post: b is true if km is inside a contig, in that
//       case end is the fw link and c is the nucleotide used for the link.
//       if b is false, end and c are not updated
//       if km is an isolated self link (e.g. 'AAA') i.e. end == km then returns false
//       deg is the degree of the end
bool ContigMapper::fwBfStep(Kmer km, Kmer& end, char& c, size_t& deg) const {
  size_t i,j;
  size_t fw_count = 0;
  //size_t k = Kmer::k;

  // check fw direction
  j = -1;
  for (i = 0; i < 4; ++i) {
    Kmer fw_rep = end.forwardBase(alpha[i]).rep();
    if (bf->contains(fw_rep)) {
      j = i;
      ++fw_count;
      if (fw_count > 1) {
        break;
      }
    }
  }

  if (fw_count != 1) {
    deg = fw_count;
    return false;
  }
  // only one k-mer in fw link
  deg = 1;

  Kmer fw = end.forwardBase(alpha[j]);

  // check bw from fw link
  size_t bw_count = 0;
  for (i = 0; i < 4; ++i) {
    Kmer bw_rep = fw.backwardBase(alpha[i]).rep();
    if (bf->contains(bw_rep)) {
      ++bw_count;
      if (bw_count > 1) {
        break;
      }
    }
  }

  assert(bw_count >= 1);
  if (bw_count != 1) {
    return false;
  }

  if (fw != km) {
    // exactly one k-mer fw, character used is c
    end = fw;
    c = alpha[j];
    return true;
  } else {
    return false;
  }

}

// use:  cc = cm.find(km)
// pre:
// post: cc is not empty if there is some info about km
//       in the contig map.
ContigMap ContigMapper::find(Kmer km) const {
  hmap_short_contig_t::const_iterator sit;
  hmap_long_contig_t::const_iterator lit;
  hmap_shortcut_t::const_iterator sc_it;

  Kmer tw = km.twin();

  if ((sit = sContigs.find(km)) != sContigs.end()) {
    return ContigMap(km, 0, 1, sit->second.size(), true ,true);
  } else if ((sit = sContigs.find(tw)) != sContigs.end()) {
    return ContigMap(tw, 0, 1, sit->second.size(), false, true);
  } else if ((lit = lContigs.find(km)) != lContigs.end()) {
    return ContigMap(km, 0, 1, lit->second->length(), true, false);
  } else if ((lit = lContigs.find(tw)) != lContigs.end()) {
    return ContigMap(tw, 0, 1, lit->second->length(), false, false);
  } else {
    if ((sc_it = shortcuts.find(km)) != shortcuts.end()) {
      lit = lContigs.find(sc_it->second.first);
      return ContigMap(lit->first, sc_it->second.second, 1, lit->second->ccov.size(), true, false); // found on fw strand
    } else if ((sc_it = shortcuts.find(tw)) != shortcuts.end()) {
      lit = lContigs.find(sc_it->second.first);
      return ContigMap(lit->first, sc_it->second.second, 1, lit->second->ccov.size(), false, false); // found on rev strand
    }
  }
  return ContigMap();
}



// use:  mapper.moveShortContigs()
// pre:  nothing
// post: all short contigs have been moved from sContigs to lContigs
//       in lContigs kmer head maps to sequence[k:] i.e. what comes after the
void ContigMapper::moveShortContigs() {
  size_t k = Kmer::k;
  lContigs.reserve(lContigs.size() + sContigs.size());
  for (hmap_short_contig_t::iterator it = sContigs.begin(); it != sContigs.end(); ) {
    string s;
    bool b;
    findContigSequence(it->first,s, b);
    assert(it->first == Kmer(s.c_str()));
    Contig *c = new Contig(s.c_str()+k, true);
    c->coveragesum = 2*(s.size() - k+1); // not 100% correct
    lContigs.insert(make_pair(it->first,c));
    sContigs.erase(it++); // note post-increment
  }
  assert(sContigs.size() == 0);
}

// use:  mapper.fixShortContigs()
// pre:
// post: all short contigs moved in method moveShortContigs have been fixed
void ContigMapper::fixShortContigs() {
  size_t k = Kmer::k;

  for (hmap_long_contig_t::iterator it = lContigs.begin(); it != lContigs.end(); ++it) {
    Contig *c = it->second;
    if (c->length() < k) { // check the strict inequality here
      CompressedSequence& seq = c->seq;
      string s = seq.toString(); // copy
      seq.reserveLength(seq.size()+k);
      seq.setSequence(it->first, k);
      seq.setSequence(s,s.size(),k);

      if (seq.size() > k) {
        for (size_t i = stride; i < seq.size() - k; i+= stride) {
          shortcuts.insert(make_pair(seq.getKmer(i), make_pair(it->first, i)));
        }
        shortcuts.insert(make_pair(seq.getKmer(seq.size()-k), make_pair(it->first, seq.size()-k)));
      }
    }
  }

  assert(checkShortcuts());
}


bool ContigMapper::checkShortcuts() {
  size_t k = Kmer::k;
  for (hmap_long_contig_t::iterator it = lContigs.begin(); it != lContigs.end(); ++it) {
    CompressedSequence& seq = it->second->seq;

    Kmer tail = seq.getKmer(seq.size()-k);
    Kmer head = seq.getKmer(0);

    if (it->first != head) {
      cout << "first != head" << endl;
      cout << "seq:  " << seq.toString() << endl;
      cout << "it:   " << it->first.toString() << endl;
      cout << "head: " << head.toString() << endl;
      return false;
    }

    if (head != tail) {
      hmap_shortcut_t::iterator sit = shortcuts.find(tail);
      if (sit == shortcuts.end()) {
        cout << "shortcut not found" << endl;
        cout << "seq:  " << seq.toString() << endl;
        cout << "tail: " << tail.toString() << endl;
        return false;
      }
      if (sit->second.second != seq.size()-k) {
        cout << "position wrong" << endl;
        cout << "seq:  " << seq.toString() << endl;
        cout << "tail: " << tail.toString() << endl;
        return false;
      }
    }
  }
  return true;
}

// use:  del = mapper.removeIsolatedContigs()
// pre:  no short contigs exist in sContigs, all contigs are full
// post: all isolated contigs, with <k k-mers or fewer have been removed
size_t ContigMapper::removeIsolatedContigs() {
  size_t rem = 0;
  size_t k = Kmer::k;

  assert(sContigs.size() == 0);
  vector<Kmer> rems;
  //typedef hmap_long_contig_t::const_iterator lit_t;
  //  for (lit_t it = lContigs.begin(); it != lContigs.end(); ++it) {
  for (auto& kv : lContigs) {
    CompressedSequence& seq = kv.second->seq;
    size_t kmerlen = kv.second->ccov.size();
    if (kmerlen >= k) {
      continue;
    }

    Kmer head = seq.getKmer(0), tail = seq.getKmer(seq.size()-k);

    size_t fw_count = 0, bw_count = 0;
    bool dummy;
    Kmer fw,bw;
    // TODO: refactor this forloop, used in more functions
    for (size_t i = 0; i < 4; i++) {
      Kmer fw = tail.forwardBase(alpha[i]);
      if (checkEndKmer(fw, dummy)) {
        fw_count++;
      }
      Kmer bw = head.backwardBase(alpha[i]);
      if (checkEndKmer(bw, dummy)) {
        bw_count++;
      }
    }

    if (fw_count == 0 && bw_count == 0) {
      rems.push_back(kv.first);
    }
  }


  for (auto& km : rems) {
    ContigMap cc = find(km);
    if (!cc.isEmpty) {
      assert(km == cc.head);
      Contig *contig = lContigs.find(cc.head)->second;
      string seq = contig->seq.toString();

      removeShortcuts(seq); // just playing it safe
      lContigs.erase(cc.head);
      delete contig;
      rem++;
    }
  }
  return rem;
}

// use:  clipped = mapper.clipTips()
// pre:  no short contigs exist in sContigs, all contigs are full
// post: all tips with length < k have been removed
size_t ContigMapper::clipTips() {
  size_t clipped = 0;
  size_t k = Kmer::k;

  assert(sContigs.size() == 0);

  vector<Kmer> clips;
  //typedef hmap_long_contig_t::const_iterator lit_t;
  //for (lit_t it = lContigs.begin(); it != lContigs.end(); ++it) {
  for (auto& kv : lContigs) {
    CompressedSequence& seq = kv.second->seq;
    size_t kmerlen = kv.second->ccov.size();
    if (kmerlen >= k) {
      continue;
    }

    Kmer head = seq.getKmer(0), tail = seq.getKmer(seq.size()-k);

    size_t fw_count = 0, bw_count = 0;
    bool dummy;
    Kmer fw_cand, bw_cand;
    for (size_t i = 0; i < 4; i++) {
      Kmer fw = tail.forwardBase(alpha[i]);
      if (checkEndKmer(fw, dummy)) {
        fw_count++;
        fw_cand = fw;
      }
      Kmer bw = head.backwardBase(alpha[i]);
      if (checkEndKmer(bw, dummy)) {
        bw_count++;
        bw_cand = bw;
      }
    }

    bool clip = false;
    if (fw_count == 0 && bw_count == 1) {
      for (size_t i = 0; i < 4; i++) {
        Kmer alt = bw_cand.forwardBase(alpha[i]);
        if (alt != head) {
          ContigMap cc = find(alt);
          if (cc.size > kmerlen) {
            clip = true;
          }
        }
      }
    }
    if (fw_count == 1 && bw_count == 0) {
      // check alternative
      for (size_t i = 0; i < 4; i++) {
        Kmer alt = fw_cand.backwardBase(alpha[i]);
        if (alt != tail) {
          ContigMap cc = find(alt);
          if (cc.size >= kmerlen) {
            clip = true;
          }
        }
      }
    }

    if (clip) {
      clips.push_back(kv.first);
    }
  }


  //for (vector<Kmer>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
  for (auto& km : clips) {
    ContigMap cc = find(km);
    if (!cc.isEmpty) {
      Contig *contig = lContigs.find(cc.head)->second;
      string seq = contig->seq.toString();

      removeShortcuts(seq); // just playing it safe
      lContigs.erase(cc.head);
      delete contig;
      clipped++;
    }
  }

  return clipped;
}

// use:  joined = mapper.joinAllContigs()
// pre:  no short contigs exist in sContigs.
// post: all contigs that could be connected have been connected
//       joined is the number of joined contigs
size_t ContigMapper::joinAllContigs() {
  size_t joined = 0;
  size_t k = Kmer::k;

  assert(sContigs.size() == 0);

  // a and b are candidates for joining
  typedef pair<Kmer, Kmer> Join_t;
  vector<Join_t> joins;

  for (hmap_long_contig_t::iterator it = lContigs.begin(); it != lContigs.end(); ++it) {
    CompressedSequence& seq = it->second->seq;
    Kmer head = seq.getKmer(0), tail = seq.getKmer(seq.size()-k);

    Kmer fw,bw;
    bool fw_dir = true,bw_dir = true;

    if (checkJoin(tail,fw,fw_dir)) {
      //cout << "Tail: " <<  tail.toString() << " -> " << fw.toString() << " " << fw_dir << endl;
      joins.push_back(make_pair(tail,fw));
    }
    if (checkJoin(head.twin(),bw,bw_dir)) {
      //cout << "Head: " <<  head.twin().toString() << " -> " << bw.toString() << " " << bw_dir << endl;
      joins.push_back(make_pair(head.twin(), bw));
    }

  }


  for (vector<Join_t>::iterator it = joins.begin(); it != joins.end(); ++it) {
    Kmer head = it->first;
    Kmer tail = it->second;

    ContigMap cHead = find(head);
    ContigMap cTail = find(tail);

    //cout << head.toString() << " -> " << tail.toString() << endl;

    if (cHead.head == cTail.head) {
      // can't join a sequence with itself, either hairPin, loop or mobius loop
      continue;
    }

    if (!cHead.isEmpty && !cTail.isEmpty) {

      // both kmers are still end-kmers
      Contig *headContig = lContigs.find(cHead.head)->second;
      Contig *tailContig = lContigs.find(cTail.head)->second;
      string headSeq = headContig->seq.toString();
      string tailSeq = tailContig->seq.toString();

      bool headDir = true;
      bool tailDir = true;

      if (head == headContig->seq.getKmer(headContig->seq.size()-k)) {
        headDir = true;
      } else if (head.twin() == headContig->seq.getKmer(0)) {
        headDir = false;
      } else {
        continue; // can't join up
      }

      if (tail == tailContig->seq.getKmer(0)) {
        tailDir = true;
      } else if (tail.twin() == tailContig->seq.getKmer(tailContig->seq.size()-k)) {
        tailDir = false;
      } else {
        continue; // can't join up
      }



      // remove shortcuts
      removeShortcuts(headSeq);
      removeShortcuts(tailSeq);


      //cout << "headdir, tailDir: " << headDir << ", " << tailDir << endl;
      if (!headDir) {
        headSeq = CompressedSequence(headSeq).rev().toString();
      }
      if (!tailDir) {
        tailSeq = CompressedSequence(tailSeq).rev().toString();
      }

      //cout << "joining" << endl << headSeq << endl;


      string joinSeq;
      joinSeq.append(headSeq);
      joinSeq.append(tailSeq, k-1, string::npos);


      //cout << string(headSeq.size()-k+1, ' ') << tailSeq << endl;
      //cout << joinSeq << endl;


      assert(headSeq.substr(headSeq.size()-k+1) == tailSeq.substr(0,k-1));

      Contig *c = new Contig(joinSeq.c_str(), true);
      c->coveragesum = headContig->coveragesum + tailContig->coveragesum;
      lContigs.erase(cHead.head);
      lContigs.erase(cTail.head);
      delete headContig;
      delete tailContig;
      Kmer cHead(joinSeq.c_str());
      lContigs.insert(make_pair(cHead,c));
      shortcuts.insert(make_pair(Kmer(joinSeq.c_str()+joinSeq.size()-k), make_pair(cHead, joinSeq.size()-k)));
      joined++;
    }

  }

  return joined;
}

// use:  r = mapper.checkJoin(a,b,dir)
// pre:  a is and endpoint
// pos:  r is true iff a->b (dir is true) or a->~b (dir is false)
//       and this is the only such pair with a or b in it
bool ContigMapper::checkJoin(Kmer a, Kmer& b, bool& dir) {
  size_t k = Kmer::k;
  size_t fw_count = 0, bw_count = 0;
  bool fw_dir, bw_dir;
  Kmer fw_cand, bw_cand;

  //cout << string(Kmer::k, '-') << endl <<  a.toString() << endl;

  for (size_t i = 0; i < 4; i++) {
    Kmer fw = a.forwardBase(alpha[i]);
    //cout << " " << fw.toString();

    if (checkEndKmer(fw,fw_dir)) {
      fw_count++;
      fw_cand = fw;
      //cout << " * ";
    }

    //cout << endl;
  }

  if (fw_count == 1) {
    ContigMap cand = find(fw_cand);
    ContigMap ac = find(a);
    if (cand.head != ac.head) { // not a self loop or hair-pin
      // no self-loop
      for (size_t j = 0; j < 4; j++) {
        Kmer bw = fw_cand.backwardBase(alpha[j]);
        //cout << bw.toString();
        if (checkEndKmer(bw.twin(), bw_dir)) {
          bw_count++;
          bw_cand = bw;
          //cout << " * ";
        }
        //cout << endl;
      }

      if (bw_count == 1) {
        // ok join up
        //CompressedSequence& ourSeq  = lContigs.find(ac.head)->second->seq;
        CompressedSequence& candSeq = lContigs.find(cand.head)->second->seq;
        Kmer candFirst = candSeq.getKmer(0);
        Kmer candLast  = candSeq.getKmer(candSeq.size()-k);

        assert(candFirst == cand.head);

        //cout << "match " << a.toString() << " -> " << fw_cand.toString() << endl;
        //cout << "candFirst: " << candFirst.toString() << endl;
        //cout << "~candLast:  " << candLast.twin().toString() << endl;

        if (candFirst == fw_cand) {
          //cout << "a->b" << endl;
          b = fw_cand;
          dir = true;
          return true;
        }

        if (candLast.twin() == fw_cand) {
          //cout << "a->~b" << endl;
          b = fw_cand;
          dir = false;
          return true;
        }
        return true;
      }
    } else {
      //cout << " self loop" << endl;
      return false;
    }
  }
  return false;
}


// use: r = mapper.checkGraphConnection(b,dir)
// pre:
// post: true iff b is an end contig in mapper and r is
//       set to true if beginning or false if b is the end
// TODO: test checkEndKmer, w.r.t. circular mapping
//       probably some problem there
bool ContigMapper::checkEndKmer(Kmer b, bool& dir) {
  ContigMap cand = find(b);
  if (cand.isEmpty) {
    return false;
  }
  size_t seqSize = lContigs.find(cand.head)->second->numKmers();
  if (cand.dist == 0) {
    dir = true;
    return true;
  } else if (cand.dist == seqSize-1) {
    dir = false;
    return true;
  } else {
    return false; // doesn't match, can this ever happen?
  }
}


// use:  split, deleted = mapper.splitAllContigs()
// post: All contigs with 1 coverage somewhere have been split where the coverage is 1
//       split is the number of contigs splitted
//       deleted is the number of contigs deleted
//       Now every contig in mapper has coverage >= 2 everywhere
pair<size_t, size_t> ContigMapper::splitAllContigs() {

  Kmer km;

  size_t k = Kmer::k;

  size_t split = 0, deleted =0 ;

  /*  size_t l_contigcount = lContigs.size();
  size_t s_contigcount = sContigs.size();
  */

  // for each short-contig
  typedef vector<pair<int,int>> split_vector_t;
  vector<string> split_contigs;
  for (hmap_short_contig_t::iterator it = sContigs.begin(); it != sContigs.end(); ) {
    // check if we should split it up
    if (! it->second.isFull()) {
      string s;
      bool selfLoop = false;
      findContigSequence(it->first,s, selfLoop);
      split_vector_t sp = it->second.splittingVector();

      if (sp.empty()) {
        deleted++;
      } else {
        split++;
      }

      if (selfLoop) {
        cout << "Splitting a self-loop, not implemented" << endl;
      }

      // remember small contigs
      // TODO: insert only middle part, if we are discarding small ones
      for (split_vector_t::iterator sit = sp.begin(); sit != sp.end(); ++sit) {
        size_t pos = sit->first;
        size_t len = sit->second - pos;
        split_contigs.push_back(s.substr(pos,len+k-1));
      }


      // erase the split contig
      sContigs.erase(it++); // note: post-increment
    } else {
      ++it;
    }
  }

  // insert short contigs
  for (vector<string>::iterator it = split_contigs.begin(); it != split_contigs.end(); ++it) {
    if (it->size() >= k) {
      //bool rev = false;
      const char *s = it->c_str();
      Kmer head = Kmer(s); //Kmer(s).rep();
      //Kmer tail = KmKmer((s + it->size()-k)).rep();
      /*if (tail < head) {
      swap(head,tail);
      rev = true;
      } */
      // insert contigs into the long contigs, so that the sequence is stored!
      Contig *cont = NULL;
      //if (!rev) {
      cont = new Contig(s,true);
      /* } else {
      string srs = CompressedSequence(s).rev().toString();
      const char *rs = srs.c_str();
      cont = new Contig(rs+k,true);
      }*/
      cont->coveragesum = 2 * (it->size()-k+1); // fake sum, TODO: keep track of this!
      lContigs.insert(make_pair(head,cont));
      if (it->size() > k) {
        size_t lastpos = it->size()-k;
        shortcuts.insert(make_pair(Kmer(s+lastpos),make_pair(head,lastpos)));
        //shortcuts.insert(make_pair(head, make_pair(s+lastpos, lastpos)));
      }
    }
  }
  split_contigs.clear();



  // long contigs
  vector<pair<string, uint64_t>> long_split_contigs;
  for (hmap_long_contig_t::iterator it = lContigs.begin(); it != lContigs.end(); ) {
    if (! it->second->ccov.isFull()) {
      const string& s = it->second->seq.toString();
      CompressedCoverage& ccov = it->second->ccov;
      pair<size_t, size_t> lowpair = ccov.lowCoverageInfo();
      size_t lowcount = lowpair.first;
      size_t lowsum = lowpair.second;
      size_t totalcoverage = it->second->coveragesum - lowsum;

      // remember pieces
      split_vector_t sp = it->second->ccov.splittingVector();
      if (sp.empty()) {
        deleted++;
      } else {
        split++;
      }

      // TODO: discard short middle pieces
      for (split_vector_t::iterator sit = sp.begin(); sit != sp.end(); ++sit) {
        size_t pos = sit->first;
        size_t len = sit->second - pos;
        long_split_contigs.push_back(make_pair(s.substr(pos,len+k-1),(totalcoverage * len)/(ccov.size() - lowcount)));
      }

      // remove shortcuts
      removeShortcuts(s);

      // erase the split contig
      delete it->second;
      it->second = NULL;
      lContigs.erase(it++); // note: post-increment
    } else {
      ++it;
    }
  }
  // insert the pieces back
  for (vector<pair<string, uint64_t>>::iterator it = long_split_contigs.begin(); it != long_split_contigs.end(); ++it) {
    if (it->first.size() >= k) {
      const char *s = it->first.c_str();

      size_t len = it->first.size();
      Kmer head = Kmer(s);
      Kmer tail = Kmer((s+len-k)).twin();

      string tmp;

      if (tail < head) {
        swap(head,tail);
        CompressedSequence c(s);
        tmp = c.rev().toString(); // TODO: create better utility methods!
        s = tmp.c_str();
      }

      // insert new contig
      Contig *cont = new Contig(s,true);
      cont->coveragesum = it->second;
      //cout << "inserting " << s << endl;
      lContigs.insert(make_pair(head, cont));

      // create new shortcuts
      for (size_t i = k; i < len-k; i+= k) {
        //cout << "short cut at " << i << endl;
        shortcuts.insert(make_pair(Kmer(s+i),make_pair(head,i)));
      }
      //cout << "final shortcut at " << (len-k) << endl;
      shortcuts.insert(make_pair(Kmer(s+len-k), make_pair(head,len-k)));
    }
  }

  long_split_contigs.clear();
  assert(checkShortcuts());
  return make_pair(split,deleted);
}



// use:  mapper.removeShortcuts(s)
// pre:  s.size >= k
// post: no shortcuts map to kmers in s
void ContigMapper::removeShortcuts(const string& s) {
  size_t k = Kmer::k;
  const char *c = s.c_str();

  for (size_t i = stride; i < s.size()-k+1; i += stride) {
    shortcuts.erase(Kmer(c+i));
  }
  shortcuts.erase(Kmer(c+s.size()-k)); // erase last k-mer

}


// use:  count2 = mapper.writeGFA(count1, graphfilename);
// pre:  the program has permissions to open graphfilename
// post: The graph has been written to the file: graphfilename
//       count2 is the number of real contigs and we assert that count1 == count2
//       if debug is true, output is written to stdout
size_t ContigMapper::writeGFA(int count1, string graphfilename, bool debug = false) {
  size_t id = 0;

  ofstream graphfile;
  ostream graph(0);
  if (!debug) {
    graphfile.open(graphfilename.c_str());
    graph.rdbuf(graphfile.rdbuf());
    assert(!graphfile.fail());
    assert(sContigs.size() == 0);
  } else {
    graph.rdbuf(cout.rdbuf()); // copy to cout
  }

  // gfa header
  graph << "H\tVN:Z:1.0\n";

  if (debug) { graph << "--- long contigs ---" << endl; }


  KmerHashTable<size_t> idmap(lContigs.size());
  //for (hmap_long_contig_t::iterator it = lContigs.begin(); it != lContigs.end(); ++it) {
  for (auto& kv : lContigs) {
    if (!debug) {assert(kv.second->ccov.isFull()); }
    id++;
    idmap.insert({kv.first,id});
    graph << "S\t" << id << "\t" << kv.second->seq.toString()
          << "\tLN:i:" << kv.second->seq.size()
          << "\tXC:i:" << kv.second->coveragesum << "\n";
    if (debug) { graph << kv.first.toString() << "\n";}
  }

  if (debug) {
    graph << "--- end contigs ---" << endl;
    graph << "--- shortcuts ---" << endl;
    for (auto& kv : shortcuts) {
      graph << kv.first.toString() << " -> " << kv.second.first.toString() << ", " << kv.second.second << endl;
    }
    graph << "--- end shortcuts ---" << endl;
  }


  size_t k = Kmer::k;

  for (auto& kv : lContigs) {
    size_t labelA = idmap.find(kv.first)->second;
    size_t labelB = 0;
    CompressedSequence& seq = kv.second->seq;

    Kmer first = seq.getKmer(0);
    Kmer last  = seq.getKmer(seq.size()-k);

    for (auto a : alpha) {
      // check for + -> +/- links
      Kmer b = last.forwardBase(a);
      ContigMap cand = find(b);
      if (!cand.isEmpty) {
        labelB = idmap.find(cand.head)->second;
        if (cand.strand) {
          // a + -> b +, output normally
          graph << "L\t" << labelA << "\t+\t" << labelB << "\t+\t" << (k-1) << "M\n";
        } else {
          // a + -> b -, only if a < b
          if (labelA <= labelB) { // if labelA == labelB we have a cycle
            graph << "L\t" << labelA << "\t+\t" << labelB << "\t-\t" << (k-1) << "M\n";
          }
        }
      }
    }

    for (auto a : alpha) {
      Kmer b = first.backwardBase(a);
      ContigMap cand = find(b);
      if (!cand.isEmpty) {
        labelB = idmap.find(cand.head)->second;
        if (cand.strand) {
          // a - -> b -, do nothing
        } else {
          // a - -> b +
          if (labelA < labelB) {
            graph << "L\t" << labelA << "\t-\t" << labelB << "\t+\t" << (k-1) << "M\n";
          }
        }
      }
    }
  }

  if (!debug) {
    graphfile.close();
  }

  return id;

}

void ContigMapper::printState() const {
  cout << string(40,'-') << endl;
  cout << "Short contigs" << endl;
  for (auto& kv : sContigs) {
    cout << "  [" << kv.first.toString() << "] -> cov = " << kv.second.toString() << endl;
  }
  cout << "Long contigs" << endl;
  for (auto& kv : lContigs) {
    cout << "  [" << kv.first.toString() << "] -> seq = " << kv.second->seq.toString() << ", cov = " << kv.second->ccov.toString() << endl;
  }

  cout << "Shortcuts" << endl;
  for (auto& kv : shortcuts) {
    cout << "  [" << kv.first.toString() << "] -> (km,pos) = (" << kv.second.first.toString() << ", " << kv.second.second << ")" << endl;
  }


}
