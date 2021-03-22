# 将 boot.S 和 main.c 链接完成的 bootloader 填充为 512-bit，限制 bootloader 大小为510-bit
# 最后 2-bit 填充 bootloader 的魔数 '\x55\xAA'，BIOS 会校验魔数确定第一个扇区是 bootloader
# 需要依赖 /usr/bin/perl

open(BB, $ARGV[0]) || die "open $ARGV[0]: $!";

binmode BB;
my $buf;
read(BB, $buf, 1000);
$n = length($buf);

if($n > 510){
	print STDERR "boot block too large: $n bytes (max 510)\n";
	exit 1;
}

print STDERR "boot block is $n bytes (max 510)\n";

$buf .= "\0" x (510-$n);
$buf .= "\x55\xAA";

open(BB, ">$ARGV[0]") || die "open >$ARGV[0]: $!";
binmode BB;
print BB $buf;
close BB;
