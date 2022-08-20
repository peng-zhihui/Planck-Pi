#!/usr/bin/perl -w
# This file was preprocessed, do not edit!
use strict;
use Debconf::Db;
use Debconf::Question;
use Debconf::Template;

my $dir = shift || '/var/lib/debconf';

Debconf::Db->load;

our %questions;
our %templates;

foreach my $thing (qw(templates debconf)) {
	if (-e "$dir/$thing.db") {
		eval qq{require "$dir/$thing.db"};
		print STDERR $@ if $@;
	}
	else {
		print STDERR "Skipping $dir/$thing.db: DNE\n";
	}
}

foreach my $t (keys %templates) {
	$templates{$t}->{_name}=$t;
}

my $skipped=0;

foreach my $item (keys %questions) {
	my @owners=grep { $_ ne '' } keys %{$questions{$item}->{owners}};
	delete $questions{$item}, next unless @owners;

	next unless defined $questions{$item}->{template}->{_name};

	my $tname=$questions{$item}->{template}->{_name};
	$skipped++, next unless defined $tname;
	my $type=$templates{$tname}->{type};
	my $question=Debconf::Question->new($item, pop(@owners), $type);
	$question->addowner($_, '') foreach @owners;
}

my %seen_templates;
foreach my $item (keys %questions) {
	my $question=Debconf::Question->get($item);
	my $tname=$questions{$item}->{template}->{_name};
	$skipped++, next unless defined $tname;
	my $template=Debconf::Template->get($tname);
	unless (defined $template) {
		$template=Debconf::Template->new($tname, $item, $templates{$tname}->{type});
	}
	unless ($seen_templates{$template}) {
		$template->clearall;
		foreach my $field (keys %{$templates{$tname}}) {
			next if $field=~/^_name/; # except this one we added above.
			$template->$field($templates{$tname}->{$field});
		}
	}
	

	if (exists $questions{$item}->{flag_isdefault}) {
		if ($questions{$item}->{flag_isdefault} eq 'false') {
		    	$question->flag('seen', 'true');
		}
		delete $questions{$item}->{flag_isdefault};
	}
	foreach my $flag (grep /^flag_/, keys %{$questions{$item}}) {
		if ($questions{$item}->{$flag} eq 'true') {
			$flag=~s/^flag_//;
			$question->flag($flag, 'true');
		}
	}
	foreach my $var (keys %{$questions{$item}->{variables}}) {
		$question->variable($var,
			$questions{$item}->{variables}->{$var});
	}
	if (exists $questions{$item}->{value} 
		and defined $questions{$item}->{value}) {
		$question->value($questions{$item}->{value});
	}

	$question->template($questions{$item}->{template}->{_name});
}

Debconf::Db->save;

if ($skipped) {
	print STDERR "While upgrading the debconf database, $skipped corrupt items were skipped.\n";
}
