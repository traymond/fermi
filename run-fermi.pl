#!/usr/bin/env perl

use strict;
use warnings;
use Getopt::Std;

&main;

sub main {
	my %opts = (e=>'fermi', t=>2, p=>'fmdef', f=>17, k=>50);
	getopts('e:t:p:Pcf:k:Adl:', \%opts);
	$opts{P} = 1 if defined($opts{c});
	$opts{A} = defined($opts{A})? "A" : "";
	$opts{l} = defined($opts{l})? "-l$opts{l}" : "";

	die(qq/
Usage:   run-fermi.pl [options] <in1.fq> [in2.fq [...]]\n
Options: -P        the input is paired
         -c        the input is collated FASTQ (two ends in the same file)
         -A        aggressive bubble popping in pairext
         -d        double the number of jobs for split index
         -e FILE   fermi executable [$opts{e}]
         -t INT    number of threads [$opts{t}]
         -p STR    prefix of output files [$opts{p}]
         -f INT    k-mer length for unique-mer filtering [$opts{f}]
         -l INT    trim reads to INT bp after error correction [inf]
         -k INT    minimum overlap [$opts{k}]
\n/) if (@ARGV == 0);

	my (@lines, $in_list, $fqs, $n_split);

	push(@lines, "FERMI=$opts{e}", "FLTUNIQ_K=$opts{f}", "UNITIG_K=$opts{k}", "");
	push(@lines, (defined $opts{P})? "all:$opts{p}.ext.mag.gz" : "all:$opts{p}.mag.gz", "");

	$in_list = join(" ", @ARGV);
	$n_split = defined($opts{d})? $opts{t} * 2 : $opts{t};

	$fqs = '';
	if (defined($opts{P}) && !defined($opts{c})) {
		die if (@ARGV % 2 != 0);
		$fqs = '';
		for (my $i = 0; $i < @ARGV; $i += 2) {
			$fqs .= "\$(FERMI) pe2cofq $ARGV[$i] ".$ARGV[$i+1]."; ";
		}
	} else {
		for my $f (@ARGV) {
			$fqs .= ($f =~ /\.gz$/)? "gzip -dc $f; " : "cat $f; ";
		}
	}
	chop($fqs); chop($fqs);
	$fqs = '(' . $fqs . ')';

	push(@lines, "# Construct the FM-index for raw sequences");
	my $pre = "$opts{p}.raw";
	push(@lines, "$pre.split.log:$in_list");
	push(@lines, "\t$fqs | \$(FERMI) splitfa - $pre $n_split 2> $pre.split.log\n");
	&build_fmd(\@lines, $n_split, $pre, '', $opts{t}); # do not trim for the initial index

	push(@lines, "# Error correction");
	push(@lines, "$opts{p}.ec.fq.gz:$opts{p}.raw.fmd");
	push(@lines, "\t$fqs | \$(FERMI) correct -".(defined($opts{P})? 'p' : '')."t $opts{t} \$< - 2> \$@.log | gzip -1 > \$@\n");

	push(@lines, "# Construct the FM-index for corrected sequences");
	$pre = "$opts{p}.ec";
	push(@lines, "$pre.split.log:$opts{p}.ec.fq.gz");
	push(@lines, "\t\$(FERMI) fltuniq -k \$(FLTUNIQ_K) \$< 2> $opts{p}.fltuniq.log | \$(FERMI) splitfa - $pre $n_split 2> \$@\n");
	&build_fmd(\@lines, $n_split, $pre, $opts{l}, $opts{t});

	if (defined($opts{P})) {
		push(@lines, "# Generate unitigs");
		push(@lines, "$opts{p}.ec.rank:$opts{p}.ec.fmd");
		push(@lines, "\t\$(FERMI) seqrank -t $opts{t} \$< > \$@ 2> \$@.log\n");
		push(@lines, "$opts{p}.mag.gz:$opts{p}.ec.rank $opts{p}.ec.fmd");
		push(@lines, "\t\$(FERMI) unitig -t $opts{t} -l \$(UNITIG_K) -r \$^ 2> \$@.log | gzip -1 > \$@\n");

		push(@lines, "# Extension using the pairing information");
		push(@lines, "$opts{p}.c0.mag.gz:$opts{p}.mag.gz");
		push(@lines, "\t\$(FERMI) clean \$< 2> \$@.log | gzip -1 > \$@");
		push(@lines, "$opts{p}.c0r.mag.gz:$opts{p}.ec.rank $opts{p}.ec.fmd $opts{p}.c0.mag.gz");
		push(@lines, "\t\$(FERMI) remap -t $opts{t} -r \$^ 2> \$@.log | gzip -1 > \$@");
		push(@lines, "$opts{p}.c1.mag.gz:$opts{p}.c0r.mag.gz");
		push(@lines, "\t\$(FERMI) clean -CAOF \$< 2> \$@.log | gzip -1 > \$@");
		push(@lines, "$opts{p}.c1r.mag.gz:$opts{p}.ec.rank $opts{p}.ec.fmd $opts{p}.c1.mag.gz");
		push(@lines, "\t\$(FERMI) remap -t $opts{t} -r \$^ 2> \$@.log | gzip -1 > \$@");
		push(@lines, "$opts{p}.ext0.fa.gz:$opts{p}.ec.fmd $opts{p}.c0r.mag.gz $opts{p}.c1r.mag.gz");
		push(@lines, qq[\t\$(FERMI) pairext -$opts{A}t $opts{t} $opts{p}.ec.fmd $opts{p}.c0r.mag.gz `perl -ne 'print "] . '$$1 $$2' . qq[\\n" if /avg = (\\S+) std = (\\S+)/' $opts{p}.c1r.mag.gz.log` 2> \$@.log | gzip -1 > \$@]);
		push(@lines, "$opts{p}.ext1.fa.gz:$opts{p}.ec.fmd $opts{p}.c1r.mag.gz");
		push(@lines, qq[\t\$(FERMI) pairext -$opts{A}t $opts{t} \$^ `perl -ne 'print "] . '$$1 $$2' . qq[\\n" if /avg = (\\S+) std = (\\S+)/' $opts{p}.c1r.mag.gz.log` 2> \$@.log | gzip -1 > \$@\n]);

		$pre = "$opts{p}.ext";
		push(@lines, "# Build FM-index for the extended sequences");
		push(@lines, "$pre.split.log:$opts{p}.ext0.fa.gz $opts{p}.ext1.fa.gz");
		push(@lines, "\tgzip -dc \$^ | \$(FERMI) splitfa - $pre $opts{t} 2> \$@\n");
		&build_fmd(\@lines, $opts{t}, $pre, ''); # do not trim, either
		push(@lines, "$opts{p}.final.fmd.log:$opts{p}.ec.fmd $opts{p}.ext.fmd");
		push(@lines, "\t\$(FERMI) merge -t $opts{t} -fo $opts{p}.final.fmd \$^ 2> \$@");
		push(@lines, "$opts{p}.ext.mag.gz:$opts{p}.final.fmd.log");
		push(@lines, "\t\$(FERMI) unitig -t $opts{t} -l \$(UNITIG_K) $opts{p}.final.fmd 2> \$@.log | gzip -1 > \$@\n");
		push(@lines, "$opts{p}.extr.mag.gz:$opts{p}.ec.fmd $opts{p}.ext.mag.gz");
		push(@lines, "\t\$(FERMI) remap -t $opts{t} \$^ 2> \$@.log | gzip -1 > \$@\n");
	} else {
		push(@lines, "# Generate unitigs");
		push(@lines, "$opts{p}.mag.gz:$opts{p}.ec.fmd");
		push(@lines, "\t\$(FERMI) unitig -t $opts{t} -l \$(UNITIG_K) \$< 2> \$@.log | gzip -1 > \$@\n");
	}

	print join("\n", @lines), "\n";
}

sub build_fmd {
	my ($lines, $t, $pre, $optl, $n_threads) = @_;
	my ($logs, $fmds) = ('', '');
	$n_threads ||= $t;
	for (0 .. $t-1) {
		my $p = sprintf("$pre.%.4d", $_);
		$logs .= "$p.fmd.log ";
		$fmds .= "$p.fmd ";
		push(@$lines, "$p.fmd.log:$pre.split.log");
		push(@$lines, "\t\$(FERMI) build $optl -fo $p.fmd $p.fq.gz 2> \$@; rm -f $p.fq.gz");
	}
	push(@$lines, "", "$pre.fmd:$logs");
	push(@$lines, "\t\$(FERMI) merge -t $n_threads -fo \$@ $fmds 2> \$@.log; rm -f $fmds\n");
}
