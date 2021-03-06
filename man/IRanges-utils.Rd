\name{IRanges-utils}

\alias{IRanges-utils}

\alias{successiveIRanges}
\alias{breakInChunks}

\alias{whichAsIRanges}

% Coercion:
\alias{asNormalIRanges}
\alias{coerce,IRanges,NormalIRanges-method}

\title{IRanges utility functions}

\description{
  Utility functions for creating or modifying \link{IRanges} objects.
}

\usage{
## Create an IRanges instance:
successiveIRanges(width, gapwidth=0, from=1)
breakInChunks(totalsize, nchunk, chunksize)

## Turn a logical vector into a set of ranges:
whichAsIRanges(x)

## Coercion:
asNormalIRanges(x, force=TRUE)
}

\arguments{
  \item{width}{
    A vector of non-negative integers (with no NAs) specifying the widths
    of the ranges to create.
  }
  \item{gapwidth}{
    A single integer or an integer vector with one less element than
    the \code{width} vector specifying the widths of the gaps separating
    one range from the next one.
  }
  \item{from}{
    A single integer specifying the starting position of the first range.
  }
  \item{totalsize}{
    A single non-negative integer. The total size of the object to break.
  }
  \item{nchunk}{
    A single positive integer. The number of chunks.
  }
  \item{chunksize}{
    A single positive integer. The size of the chunks (last chunk might be
    smaller).
  }
  \item{x}{
    A logical vector for \code{whichAsIRanges}.

    An \link{IRanges} object for \code{asNormalIRanges}.
  }
  \item{force}{
    \code{TRUE} or \code{FALSE}. Should \code{x} be turned into a
    \link{NormalIRanges} object even if \code{isNormal(x)} is \code{FALSE}?
  }
}

\details{
  \code{successiveIRanges} returns an \link{IRanges} instance containing
  the ranges that have the widths specified in the \code{width} vector
  and are separated by the gaps specified in \code{gapwidth}.
  The first range starts at position \code{from}.
  When \code{gapwidth=0} and \code{from=1} (the defaults), the returned
  IRanges can be seen as a partitioning of the 1:sum(width) interval.
  See \code{?Partitioning} for more details on this.

  \code{breakInChunks} returns a \link{PartitioningByEnd} object
  describing the "chunks" that result from breaking a vector-like object
  of length \code{totalsize} in the chunks described by \code{nchunk} or
  \code{chunksize}.

  \code{whichAsIRanges} returns an \link{IRanges} instance containing all of
  the ranges where \code{x} is \code{TRUE}.

  If \code{force=TRUE} (the default), then \code{asNormalIRanges} will
  turn \code{x} into a \link{NormalIRanges} instance by reordering and
  reducing the set of ranges if necessary (i.e. only if \code{isNormal(x)}
  is \code{FALSE}, otherwise the set of ranges will be untouched).
  If \code{force=FALSE}, then \code{asNormalIRanges} will turn \code{x}
  into a \link{NormalIRanges} instance only if \code{isNormal(x)} is
  \code{TRUE}, otherwise it will raise an error.
  Note that when \code{force=FALSE}, the returned object is guaranteed
  to contain exactly the same set of ranges than \code{x}.
  \code{as(x, "NormalIRanges")} is equivalent to \code{asNormalIRanges(x, force=TRUE)}.
}

\author{Hervé Pagès}

\seealso{
  \itemize{
    \item \link{IRanges} objects.

    \item \link{Partitioning} objects.

    \item \code{\link{equisplit}} for splitting a list-like object into
          a specified number of partitions.

    \item \link{intra-range-methods} and \link{inter-range-methods}
          for intra range and inter range transformations.

    \item \link{setops-methods} for performing set operations on
          \link{IRanges} objects.

    \item \code{\link{solveUserSEW}}

    \item \code{\link{successiveViews}}
  }
}

\examples{
vec <- as.integer(c(19, 5, 0, 8, 5))

successiveIRanges(vec)

breakInChunks(600999, chunksize=50000)  # chunks of size 50000 (last
                                        # chunk is smaller)

whichAsIRanges(vec >= 5)

x <- IRanges(start=c(-2L, 6L, 9L, -4L, 1L, 0L, -6L, 10L),
             width=c( 5L, 0L, 6L,  1L, 4L, 3L,  2L,  3L))
asNormalIRanges(x)  # 3 non-empty ranges ordered from left to right and
                    # separated by gaps of width >= 1.

## More on normality:
example(`IRanges-class`)
isNormal(x16)                        # FALSE
if (interactive())
    x16 <- asNormalIRanges(x16)      # Error!
whichFirstNotNormal(x16)             # 57
isNormal(x16[1:56])                  # TRUE
xx <- asNormalIRanges(x16[1:56])
class(xx)
max(xx)
min(xx)
}

\keyword{utilities}
