#!/usr/bin/mawk -f

# primes.awk
#
#  mawk -f primes.awk  [START]  STOP
# find all primes    between 2 and STOP
#       or START and STOP
#



function usage()
{ ustr = sprintf("usage: %s  [start] stop", ARGV[0])
  system( "echo " ustr) 
  exit 1
}


BEGIN { if (ARGC == 1 || ARGC > 3 ) usage()
        if ( ARGC == 2 )  { start = 2  ; stop = ARGV[1]+0 }
	else
	if ( ARGC == 3 )  { start = ARGV[1]+0 ; stop = ARGV[2]+0 }

   if ( start < 2 ) start = 2
   if ( stop < start ) stop = start

   prime[ p_cnt = 1 ] =  3  # keep primes in prime[]

# keep track of integer part of square root by adding
# odd integers 
   odd = test = 5
   root = 2
   squares = 9

   
while ( test <= stop )
{
   if ( test >= squares )
   { root++
     odd += 2
     squares += odd 
   }

   flag = 1
   for ( i = 1 ; prime[i] <= root ; i++ )
   	if ( test % prime[i] == 0 )  #  not prime
	{ flag = 0 ; break }

   if ( flag )  prime[ ++p_cnt ] = test

   test += 2
}

prime[0] = 2

for( i = 0 ; prime[i] < start ; i++)  ;

for (  ;  i <= p_cnt ; i++ )  print prime[i]

}


     
