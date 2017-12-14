/*
    dstcnv - DST encoded DSDIFF to decoded DSDIFF file converter
    Copyright (c) 2017 by Takayuki Nakano
 
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

 ------------------------------------------------------------------------------

  History :
    2017/12/05 : v0.9.0 - Initial release
*/

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include "conststr.h"
#include "dst_decoder.h"

#ifdef	_WIN32
#include <windows.h>
#define PATH_DELIM		'\\'
#ifdef	__MINGW32__
#define off_t			off64_t
#define	fseeko			fseeko64
#define	ftello			ftello64
#endif
#else
#define PATH_DELIM		'/'
#endif

#define VERSION			"0.9"
#define VERSION_STR		"dstcnv version " VERSION " (c) 2017, Takayuki Nakano(JRS)"

//-----------------------------------------------------------------------------
//   Global variables
//-----------------------------------------------------------------------------
extern char		*optarg;
extern int		optind, opterr, optopt;

FILE			*fpr = NULL;
FILE			*fpw = NULL;
char			program[ 64 ];
char			infile[ PATH_MAX ];
char			outfile[ PATH_MAX ];
uint8_t			quiet_mode = 0;
uint32_t		num_channels = 0;
uint32_t		num_frames = 0;
uint32_t		frequency = 0;
uint64_t		dsd_size = 0;
dst_decoder_t	*dstdec = NULL;

//-----------------------------------------------------------------------------
//   Utilities for endian convert
//-----------------------------------------------------------------------------
uint32_t		(*bswap32)( uint32_t );
uint64_t		(*bswap64)( uint64_t );

uint32_t swap_dword( uint32_t x )
{
	return( ( x & 0xff000000UL ) >> 24 |
			( x & 0x00ff0000UL ) >> 8  |
			( x & 0x0000ff00UL ) << 8  |
			( x & 0x000000ffUL ) << 24 );
}

uint64_t swap_qword( uint64_t x )
{
	return( ( x & 0xff00000000000000ULL ) >> 56 |
			( x & 0x00ff000000000000ULL ) >> 40 |
			( x & 0x0000ff0000000000ULL ) >> 24 |
			( x & 0x000000ff00000000ULL ) >> 8  |
			( x & 0x00000000ff000000ULL ) << 8  |
			( x & 0x0000000000ff0000ULL ) << 24 |
			( x & 0x000000000000ff00ULL ) << 40 |
			( x & 0x00000000000000ffULL ) << 56 );
}

uint32_t swap_none32( uint32_t x ) { return( x ); }
uint64_t swap_none64( uint64_t x ) { return( x ); }

int init_swapper( void )
{
	union {
		char	b[ 4 ];
		int		d;
    } chk;
	int		rc = 0;

	memcpy( chk.b, "\x01\x02\x03\x04", 4 );
	switch( chk.d ) {
		case 0x04030201:
			bswap32 = swap_dword;
			bswap64 = swap_qword;
			break;
		case 0x01020304:
			bswap32 = swap_none32;
			bswap64 = swap_none64;
			break;
		default:
			rc = -1;
			break;
	}

	return( rc );
}

//-----------------------------------------------------------------------------
//   Fatal error handler
//-----------------------------------------------------------------------------
void fatal( const char *fmt, ... )
{
	va_list	ap;

	va_start( ap, fmt );
	vfprintf( stderr, fmt, ap );
	fprintf( stderr, "\n" );
	va_end( ap );
	if( dstdec ) dst_decoder_destroy( dstdec );
	if( fpr ) fclose( fpr );
	if( fpw ) fclose( fpw );
	if( *outfile ) unlink( outfile );

	exit( EXIT_FAILURE );
}

//-----------------------------------------------------------------------------
//   Print command usage
//-----------------------------------------------------------------------------
void usage( void )
{
	printf( "usage: %s [-q] [-o output-file] dst-encoded-dsdiff-file | -h | -V\n", program );
}

//-----------------------------------------------------------------------------
//   Print option help
//-----------------------------------------------------------------------------
void help( void )
{
	usage();
	printf( "\n-- option help --\n\n" );
	printf( "-q : Quiet mode (except errors)\n" );
	printf( "-o : Specify output file name (default: <input-filename>_dec.dff)\n" );
	printf( "-h : Show option help\n" );
	printf( "-V : Show version information\n" );
}

//-----------------------------------------------------------------------------
//   DST decoder thread callbacks
//-----------------------------------------------------------------------------
void decode_callback( uint8_t *data, size_t size, void *userdata )
{
	fwrite( data, 1, size, fpw );
	fflush( fpw );
}

void error_callback( int cnt, int code, const char *msg, void *userdata )
{
	fatal( "\nError: invalid DST data in frame %d, cannot process continuously.", cnt );
}

//-----------------------------------------------------------------------------
//   Analyze property chunk
//-----------------------------------------------------------------------------
void do_process_property_chunk( void )
{
	uint64_t	chunk_size;
	uint64_t	w_chunk_size;
	uint8_t		*chunk_data, *p;
	uint8_t		chunk_ID[ 4 ];
	uint8_t		buf[ 16 ];
	off_t		prop_size;
	off_t		cursor;

	fread( &chunk_size, 1, 8, fpr );
	w_chunk_size = bswap64( chunk_size ) + 4;		// +4 is difference of
													// CMPR chunk size between
													// DST and DSD.
	w_chunk_size = bswap64( w_chunk_size );
	fwrite( &w_chunk_size, 1, 8, fpw );

	fread( buf, 1, 4, fpr );						// 'SND '
	fwrite( buf, 1, 4, fpw );

	prop_size = (off_t)bswap64( chunk_size ) - 4;	// -4 is length of 'SND '
	cursor = ftello( fpr );
	while( ftello( fpr ) - cursor < prop_size ) {
		fread( chunk_ID, 1, 4, fpr );
		fread( &chunk_size, 1, 8, fpr );
		w_chunk_size = bswap64( chunk_size );
		if( ( chunk_data = (uint8_t *)malloc( w_chunk_size ) ) == NULL ) {
			fatal( "Insufficient memory in PROP chunk copy." );
		}
		fread( chunk_data, 1, w_chunk_size, fpr );

		if( memcmp( chunk_ID, "CHNL", 4 ) == 0 ) {
			num_channels = (uint32_t)chunk_data[ 1 ];
		}
		else if( memcmp( chunk_ID, "FS  ", 4 ) == 0 ) {
			memcpy( (uint8_t *)&frequency, chunk_data, 4 );
			frequency = bswap32( frequency );
		}
		else if( memcmp( chunk_ID, "CMPR", 4 ) == 0 ) {
			if( memcmp( chunk_data, "DSD ", 4 ) == 0 ) {
				fatal( "Input file is not DST encoded." );
			}
			w_chunk_size = 0x14;
			chunk_size = bswap64( w_chunk_size );
			p = (uint8_t *)realloc( chunk_data, w_chunk_size );
			if( p == NULL ) {
				fatal( "Insufficient memory in CMPR chunk copy." );
			}
			chunk_data = p;
			memcpy( chunk_data, "DSD \x0enot compressed\x00", w_chunk_size );
		}
		fwrite( chunk_ID, 1, 4, fpw );
		fwrite( &chunk_size, 1, 8, fpw );
		fwrite( chunk_data, 1, w_chunk_size, fpw );
		fflush( fpw );
		free( chunk_data );
	}

	return;
}

//-----------------------------------------------------------------------------
//   Analyze DST sound data chunk
//-----------------------------------------------------------------------------
void do_process_dst_sound_data_chunk( void )
{
	uint64_t	chunk_size;
	uint64_t	dst_size;
	uint64_t	sample_count;
	uint32_t	n = 1;
	uint32_t	min;
	uint8_t		chunk_ID[ 4 ];
	uint8_t		buf[ 16 ];
	uint8_t		*frame_data;
	size_t		frame_size;
	off_t		cursor;
	double		duration;
#ifdef	_WIN32
	COORD		c;
	HANDLE		h;
	CONSOLE_SCREEN_BUFFER_INFO	sc;

	h = GetStdHandle( STD_OUTPUT_HANDLE );
#endif

	fread( &chunk_size, 1, 8, fpr );
	dst_size = bswap64( chunk_size );

	memcpy( buf, "DSD \x00\x00\x00\x00\x00\x00\x00\x00", 12 );
	fwrite( buf, 1, 12, fpw );						// dummy size
	fflush( fpw );

	cursor = ftello( fpr );
	while( ftello( fpr ) - cursor < dst_size ) {
		fread( chunk_ID, 1, 4, fpr );
		fread( &chunk_size, 1, 8, fpr );

		if( memcmp( chunk_ID, "FRTE", 4 ) == 0 ) {
			// Found DST frame information chunk
			fread( &num_frames, 1, 4, fpr );		// number of DST frames
			num_frames = bswap32( num_frames );
			dsd_size = ( MAX_DSDBITS_INFRAME / 8 * num_channels ) * num_frames;
			if( quiet_mode == 0 ) {
				sample_count = dsd_size * 8 / num_channels;
				duration = (double)sample_count / (double)frequency;
				min = (uint32_t)duration / 60;

				printf( "Source file  : %s\n", infile );
				printf( "Output file  : %s\n", outfile );
				printf( "Sample freq. : %d Hz\n", frequency );
				printf( "Channels     : %d (%s)\n", num_channels,
						num_channels == 2 ? "Stereo" : "Multi ch." );
				printf( "Duration     : %02d:%06.3f\n",
						min, duration - min * 60 );
#ifdef	_WIN32
				GetConsoleScreenBufferInfo( h, &sc );
				c.X = 0;
				c.Y = sc.dwCursorPosition.Y;
#endif
			}
			fread( buf, 1, 2, fpr );				// DST frame rate per sec.
		}
		else if( memcmp( chunk_ID, "DSTF", 4 ) == 0 ) {
			// Found DST frame data chunk
			frame_size = bswap64( chunk_size );
			if( ( frame_data = (uint8_t *)malloc( frame_size ) ) == NULL ) {
				fatal( "Insufficient memory in DST decode." );
			}
			memset( frame_data, 0, frame_size );
			fread( frame_data, 1, frame_size, fpr );
			if( frame_size % 2 ) {
				(void)fgetc( fpr );					// Read padding byte
			}
			dst_decoder_decode( dstdec, frame_data, frame_size );
			free( frame_data );

			if( quiet_mode == 0 ) {
#ifdef	_WIN32
				SetConsoleCursorPosition( h, c );
				printf( "Decoding DST frames %d of %d ", n, num_frames );
#else
				printf( "\033[GDecoding DST frames %d of %d ", n, num_frames );
#endif
				fflush( stdout );
				n++;
			}
		}
		else if( memcmp( chunk_ID, "DSTC", 4 ) == 0 ) {
			// Skip 'DSTC' chunk with padding byte
			chunk_size = bswap64( chunk_size );
			chunk_size += ( chunk_size % 2 );
			fseeko( fpr, (off_t)chunk_size, SEEK_CUR );
		}
		else {
			// Other chunk than those above, skip chunk.
			fseeko( fpr, (off_t)bswap64( chunk_size ), SEEK_CUR );
		}
	}

	return;
}

//-----------------------------------------------------------------------------
//   DST decode procedure main
//-----------------------------------------------------------------------------
void do_decode_DST( void )
{
	uint64_t	chunk_size;
	uint64_t	w_chunk_size;
	uint64_t	file_size;
	uint8_t		*chunk_data;
	uint8_t		chunk_ID[ 4 ];
	uint8_t		buf[ 16 ];
	off_t		size_pos;

	fread( chunk_ID, 1, 4, fpr );
	if( memcmp( chunk_ID, "FRM8", 4 ) != 0 ) {
		fatal( "Invalid form DSD chunk header." );
	}
	fwrite( chunk_ID, 1, 4, fpw );

	fread( buf, 1, 12, fpr );					// "12" is sizeof( ckDataSize )
	fwrite( buf, 1, 12, fpw );					// + sizeof( formType )

	while( fread( chunk_ID, 1, 4, fpr ) == 4 ) {
		if( memcmp( chunk_ID, "PROP", 4 ) == 0 ) {
			// Found property chunk
			fwrite( chunk_ID, 1, 4, fpw );
			do_process_property_chunk();
			dstdec = dst_decoder_create( num_channels, decode_callback,
										 error_callback, NULL );
			if( dstdec == NULL ) {
				fatal( "DST decoder cannot be initialized." );
			}
		}
		else if( memcmp( chunk_ID, "DSD ", 4 ) == 0 ) {
			// DST encoded dsdiff does not have 'DSD' sound data chunk.
			fatal( "Input file is not DST encoded." );
		}
		else if( memcmp( chunk_ID, "DST ", 4 ) == 0 ) {
			// Found DST sound data chunk
			size_pos = ftello( fpw ) + 4;		// File offset to store decoded
												// DSD size 
			do_process_dst_sound_data_chunk();
			dst_decoder_destroy( dstdec );
			dsd_size = bswap64( dsd_size );
			fseeko( fpw, size_pos, SEEK_SET );
			fwrite( &dsd_size, 1, 8, fpw );		// Store DSD size
			fseeko( fpw, 0, SEEK_END );
		}
		else if( memcmp( chunk_ID, "DSTI", 4 ) == 0 ) {
			// Skip 'DSTI' chunk because it's not needed for DSD data.
			fread( &chunk_size, 1, 8, fpr );
			fseeko( fpr, (off_t)bswap64( chunk_size ), SEEK_CUR );
		}
		else {
			// Other chunk than those above, copy original chunk.
			fwrite( chunk_ID, 1, 4, fpw );
			fread( &chunk_size, 1, 8, fpr );
			fwrite( &chunk_size, 1, 8, fpw );
			w_chunk_size = bswap64( chunk_size );
			if( ( chunk_data = (uint8_t *)malloc( w_chunk_size ) ) == NULL ) {
				fatal( "Insufficient memory in FRM8 chunk copy." );
			}
			fread( chunk_data, 1, w_chunk_size, fpr );
			fwrite( chunk_data, 1, w_chunk_size, fpw );
			fflush( fpw );
			free( chunk_data );
		}
	}

	// After processing all chunks, set 'FRM8' chunk size calcurated by finally
	// output file size.
	file_size = ftello( fpw ) - 12;				// 12 is sizeof( ckID ) +
												// sizeof( ckDataSize )
	file_size = bswap64( file_size );
	fseeko( fpw, 4, SEEK_SET );
	fwrite( &file_size, 1, 8, fpw );
	fflush( fpw );

	if( quiet_mode == 0 ) {
		printf( "\nDone.\n" );
	}

	return;
}

//-----------------------------------------------------------------------------
//   main()
//-----------------------------------------------------------------------------
int main( int argc, char *argv[] )
{
	char	ex_outfile[ PATH_MAX ];
	char	*p;
	FILE	*ftmp;
	int		ch;

	//----- Program initialize ------------------------------------------------
	if( init_swapper() < 0 ) {
		fatal( "Unrecognized processor error.\n" );
	}
	p = strrchr( argv[ 0 ], PATH_DELIM );
	strcpy( program, p ? p + 1 : argv[ 0 ] );

	memset( infile, 0, PATH_MAX );
	memset( outfile, 0, PATH_MAX );
	memset( ex_outfile, 0, PATH_MAX );

	//----- Argument check ----------------------------------------------------
	while( ( ch = getopt( argc, argv, "Vqho:" ) ) != -1 ) {
		switch( ch ) {
			case 'V':
				printf( "%s\n", VERSION_STR );
				exit( EXIT_SUCCESS );
			case 'q':
				quiet_mode = 1;
				break;
			case 'h':
				help();
				exit( EXIT_SUCCESS );
			case 'o':
				strcpy( ex_outfile, optarg );
				break;
			case '?':
				usage();
				exit( EXIT_FAILURE );
		}
	}
	if( argc == optind ) {
		usage();
		exit( EXIT_FAILURE );
	}

	//----- Open input DST encoded dsdiff file --------------------------------
	strcpy( infile, argv[ argc - 1 ] );
	if( strlen( infile ) < 5 ) {
		fatal( "Input file is not DSDIFF file." );
	}
	p = (char *)( infile + strlen( infile ) - 4 );
	if( strcasecmp( p, ".dff" ) != 0 ) {
		fatal( "Input file is not DSDIFF file." );
	}
	if( ( fpr = fopen( infile, "rb" ) ) == NULL ) {
		fatal( "Cannot open %s", infile );
	}

	//----- Open output decoded dsdiff file -----------------------------------
	if( *ex_outfile != '\0' ) {
		strcpy( outfile, ex_outfile );
	}
	else {
		strcpy( outfile, infile );
		p = (char *)( outfile + strlen( outfile ) - 4 );
		*p = '\0';
		strcat( outfile, "_dec.dff" );
	}
	if( ( ftmp = fopen( outfile, "rb" ) ) != NULL ) {
		fclose( ftmp );
		if( quiet_mode == 0 ) {
			printf( "Output file '%s' exists already. Overwrite ? (y/n) ",
					outfile );
			ch = getchar();
			if( ch != 'y' && ch != 'Y' ) {
				printf( "Execution canceled.\n" );
				fclose( fpr );
				return( 0 );
			}
			if( unlink( outfile ) == -1 ) {
				fatal( "Cannot overwrite %s", outfile );
			}
		}
	}
	if( ( fpw = fopen( outfile, "wb" ) ) == NULL ) {
		fatal( "Cannot open %s", outfile );
	}

	//----- Execute decode procedure ------------------------------------------
	do_decode_DST();

	//----- Epilogue ----------------------------------------------------------
	fclose( fpr );
	fclose( fpw );

	return( 0 );
}
