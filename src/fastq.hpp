#ifndef FASTQ_H
#define FASTQ_H

#include <stdio.h>
#include <string.h>
#include <zlib.h>


#include <vector>
#include <string>

#include "Common.hpp"

#ifndef KSEQ_INIT_READY
#define KSEQ_INIT_READY
#include "kseq.h"
KSEQ_INIT(gzFile, gzread);
#endif

// initialize kseq structures
//KSEQ_INIT(gzFile, gzread);


class FastqFile {
 public:
  FastqFile(const vector<string> fnames);

  ~FastqFile();

  void close();
  void reopen();
  int read_next(char *read, size_t *read_len, string &seq, size_t *seq_len, unsigned int *file_id, char *qual = NULL);

  vector<string>::const_iterator fnit; // Current filename
  unsigned int file_no;

 private:
  vector<string>::const_iterator open_next(); // Method

  vector<string> fnames; // All fasta/fastq files
  gzFile fp;
  kseq_t *kseq;
};

#endif // FASTQ_H
