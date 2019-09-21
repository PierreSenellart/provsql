#!/usr/bin/perl

use strict;
use warnings;

# This script is used to generate example tables similar to the one in
# setup.sql

print "CREATE TABLE person(id INT PRIMARY KEY, name TEXT NOT NULL, date_of_birth DATE, height SMALLINT);\n";
print "CREATE TABLE room(id INT PRIMARY KEY, name TEXT NOT NULL, area SMALLINT);\n";
print "CREATE TABLE sightings(time TIME NOT NULL, person INT REFERENCES person(id) NOT NULL, room INT REFERENCES room(id) NOT NULL, witness INT REFERENCES person(id));\n";
print "CREATE TABLE reliability(person INT REFERENCES person(id) PRIMARY KEY,score float NOT NULL);\n";

my @names=qw(
  Titus  
  Norah  
  Ginny  
  Demetra  
  Sheri  
  Karleen  
  Daisey  
  Audrey  
  Alaine  
  Edwin  
  Shelli  
  Santina  
  Bart  
  Harriette  
  Jody  
  Theodora  
  Roman  
  Jack  
  Daphine  
  Kyra  
);

foreach(my $i=0;$i<@names;++$i) {
  my $year=int(rand(60))+1950;
  my $month=int(rand(12))+1;
  my $day=int(rand(28))+1;
  $month="0$month" if $month<10;
  $day="0$day" if $day<10;
  my $height=rand(50)+160;
  my $reliability=rand()/2+0.5;

  print "INSERT INTO person VALUES($i,'$names[$i]','$year-$month-$day',$height);\n";
  print "INSERT INTO reliability VALUES($i,$reliability);\n";
}

my @rooms=(
  "Dining room",
  "Blue bedroom",
  "Red bedroom",
  "Yellow bedroom",
  "Green bedroom",
  "Living room",
  "Kitchen",
  "First bathroom",
  "Second bathroom",
  "Library"
);

foreach(my $j=0;$j<@rooms;++$j) {
  my $area=int(rand(30))+8;

  print "INSERT INTO room VALUES($j,'$rooms[$j]',$area);\n"
}

foreach(my $i=0;$i<@names;++$i) {
  my $nb_observations=int(rand()*20)+1;
  foreach(my $k=0;$k<$nb_observations;++$k) {
    my $time=int(rand()*48);
    $time=sprintf("%02d",$time/2).":".($time%2?"30":"00");
    my $person;
    do {
      $person=int(rand()*@names);
    } while($person==$i);
    my $room=int(rand()*@rooms);
    print "INSERT INTO sightings VALUES('$time',$person,$room,$i);\n";
  }
}
