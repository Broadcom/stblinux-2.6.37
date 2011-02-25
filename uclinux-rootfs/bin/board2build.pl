#!/usr/bin/perl -w

my $board = $ARGV[0];

if(defined($ARGV[1])) {
	# grudgingly accept: board2build.pl BOARD REV
	$board .= $ARGV[1];
}

if(! defined($board)) {
	print "usage: board2build.pl <boardname>\n";
	print "\n";
	print "examples:\n";
	print "  board2build.pl 97456d0\n";
	print "  board2build.pl 97466b0\n";
	print "  board2build.pl 97420a0\n";
	exit 0;
}

$board =~ tr/A-Z/a-z/;

$board =~ s/7420dvr2/7420/;
$board =~ s/7420dbs/7420/;
$board =~ s/7420cb([^0-9])/7420$1/;
$board =~ s/7420c([^0-9])/7420$1/;
$board =~ s/7410c([^0-9])/7410$1/;

if($board =~ m/^9?([0-9]{4,5})([a-z])([0-9])$/) {
	$chip = $1;
	$rev = $2."0";
} elsif($board =~ m/^9?([0-9]{4,5})$/) {
	$chip = $1;
	$rev = "a0";
} else {
	print "Invalid format: $board\n";
	exit 1;
}

$combo = $chip.$rev;

if($board =~ m/^97455/) {
	$chip = "7401";
} elsif($board =~ m/^97456/) {
	$chip = "7400";
} elsif($board =~ m/^97458/) {
	$chip = "7403";
} elsif($board =~ m/^97459/) {
	$chip = "7405";
}

if($chip eq "3549" || $chip eq "3556") {
	$chip = "3548";
	$rev = "b0";
} elsif($combo eq "7413a0" || $combo eq "7414a0") {
	$chip = "7405";
	$rev = "b0";
} elsif($combo eq "7413b0" || $combo eq "7414b0") {
	$chip = "7405";
	$rev = "d0";
} elsif($chip eq "7466") {
	$chip = "7405";
	$rev = "b0";
} elsif($combo eq "7205a0" || $combo eq "7206a0") {
	$chip = "7405";
	$rev = "b0";
} elsif($combo eq "7205b0" || $combo eq "7206b0") {
	$chip = "7405";
	$rev = "d0";
} elsif($chip eq "7213" || $chip eq "7214") {
	$chip = "7405";
	$rev = "d0";
} elsif($chip eq "7336") {
	$chip = "7335";
	$rev = "b0";
} elsif($chip eq "7406") {
	$chip = "7405";
} elsif($chip eq "7324") {
	$chip = "7325";
} elsif($chip eq "7402" || $chip eq "7451") {
	$chip = "7401";
} elsif($chip eq "7404" || $chip eq "7452") {
	$chip = "7403";
} elsif($chip eq "3320" || $chip eq "7409" || $chip eq "7410") {
	$chip = "7420";
} elsif($chip eq "7208" || $chip eq "7469") {
	$chip = "7468";
} elsif($chip eq "7119" || $chip eq "7019" || $chip eq "7116" ||
		$chip eq "7117") {
	$chip = "7125";
} elsif($chip eq "7350") {
	$chip = "7340";
} elsif($chip eq "7352") {
	$chip = "7342";
} elsif($chip eq "7354") {
	$chip = "7344";
} elsif($chip eq "7356") {
	$chip = "7346";
} elsif($chip eq "7421") {
	$chip = "7425";
}

if($chip eq "7125" && $rev eq "d0") {
	$rev = "c0";
}

if($chip eq "7418" && $rev eq "a0") {
	$chip = "7344";
}

if($chip eq "7422" && $rev ne "a0") {
	$chip = "7425";
}

print "${chip}${rev}\n";

exit 0;
