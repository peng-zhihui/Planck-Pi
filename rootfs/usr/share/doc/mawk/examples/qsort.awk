#!/usr/bin/mawk -f

# qsort text files
#

function middle(x,y,z)  #return middle of 3
{
  if ( x <= y )  
  { if ( z >= y )  return y
    if ( z <  x )  return x
    return z
  }

  if ( z >= x )  return x
  if ( z <  y )  return y
  return z
}


function  isort(A , n,    i, j, hold)
{
  # if needed a sentinal at A[0] will be created

  for( i = 2 ; i <= n ; i++)
  {
    hold = A[ j = i ]
    while ( A[j-1] > hold )
    { j-- ; A[j+1] = A[j] }

    A[j] = hold
  }
}


# recursive quicksort
function  qsort(A, left, right    ,i , j, pivot, hold)
{
  
  pivot = middle(A[left], A[int((left+right)/2)], A[right])

  i = left
  j = right

  while ( i <= j )
  {
    while ( A[i] < pivot )  i++ 
    while ( A[j] > pivot )  j--

    if ( i <= j )
    { hold = A[i]
      A[i++] = A[j]
      A[j--] = hold
    }
  }

  if ( j - left > BLOCK )  qsort(A,left,j)
  if ( right - i > BLOCK )  qsort(A,i,right)
}

BEGIN { BLOCK = 5 }


{ line[NR] = $0 ""   # sort as string
}

END  {

  if ( NR > BLOCK )  qsort(line, 1, NR)

  isort(line, NR)

  for(i = 1 ; i <= NR ; i++) print line[i]
}
  



    
