#!/usr/bin/perl
# Copyright 2003 Bryan Ford
# Distributed under the GNU General Public License.
#
# Usage: mergedep <main-depfile> [<new-depfiles> ...]
#
# 这个脚本将命令行上指定的所有 <new-defiles> 的内容合并到单个文件 <main-defile> 中.(不存在则新建)
# 依赖项在 <new-depfiles> 中会覆盖 <main-depfile> 中相同目标的任何现有依赖项
# 更新 <main-depfile> 之后，删除 <new-depfiles> .
#
# 通常使用 GCC -MD 选项生成 <new-depfiles>，而 <main-depfile> 通常包含在 Makefile 中
# 如下面的 GNU make 所示:
#
#	.deps: $(wildcard *.d)
#		perl mergedep $@ $^
#	-include .deps
#
# 该脚本正确地处理每个<new-depfile>的多个依赖项，包括没有目标的依赖项，因此它与GCC3的-MP选项兼容.
#

sub readdeps {
	my $filename = shift;

	open(DEPFILE, $filename) or return 0;
	while (<DEPFILE>) {
		if (/([^:]*):([^\\:]*)([\\]?)$/) {
			my $target = $1;
			my $deplines = $2;
			my $slash = $3;
			while ($slash ne '') {
				$_ = <DEPFILE>;
				defined($_) or die
					"Unterminated dependency in $filename";
				/(^[ \t][^\\]*)([\\]?)$/ or die
					"Bad continuation line in $filename";
				$deplines = "$deplines\\\n$1";
				$slash = $2;
			}
			# print "DEPENDENCY [[$target]]: [[$deplines]]\n";
			$dephash{$target} = $deplines;
		} elsif (/^[#]?[ \t]*$/) {
			# 忽略空行和注释
		} else {
			die "Bad dependency line in $filename: $_";
		}
	}
	close DEPFILE;
	return 1;
}


if ($#ARGV < 0) {
	print "Usage: mergedep <main-depfile> [<new-depfiles> ..]\n";
	exit(1);
}

%dephash = ();

# 读取主要依赖文件
$maindeps = $ARGV[0];
readdeps($maindeps);

# 读取并合并新依赖文件
foreach $i (1 .. $#ARGV) {
	readdeps($ARGV[$i]) or die "Can't open $ARGV[$i]";
}

# 更新主依赖文件
open(DEPFILE, ">$maindeps.tmp") or die "Can't open output file $maindeps.tmp";
foreach $target (keys %dephash) {
	print DEPFILE "$target:$dephash{$target}";
}
close DEPFILE;
rename("$maindeps.tmp", "$maindeps") or die "Can't overwrite $maindeps";

# 最后，删除新依赖文件
foreach $i (1 .. $#ARGV) {
	unlink($ARGV[$i]) or print "Error removing $ARGV[$i]\n";
}

