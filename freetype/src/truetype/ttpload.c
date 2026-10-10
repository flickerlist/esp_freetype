/****************************************************************************
 *
 * ttpload.c
 *
 *   TrueType-specific tables loader (body).
 *
 * Copyright (C) 1996-2024 by
 * David Turner, Robert Wilhelm, and Werner Lemberg.
 *
 * This file is part of the FreeType project, and may only be used,
 * modified, and distributed under the terms of the FreeType project
 * license, LICENSE.TXT.  By continuing to use, modify, or distribute
 * this file you indicate that you have read the license and
 * understand and accept it fully.
 *
 */


#include <freetype/internal/ftdebug.h>
#include <freetype/internal/ftobjs.h>
#include <freetype/internal/ftstream.h>
#include <freetype/tttags.h>

#include "ttpload.h"

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
#include "ttgxvar.h"
#endif

#include "tterrors.h"


  /**************************************************************************
   *
   * The macro FT_COMPONENT is used in trace mode.  It is an implicit
   * parameter of the FT_TRACE() and FT_ERROR() macros, used to print/log
   * messages during execution.
   */
#undef  FT_COMPONENT
#define FT_COMPONENT  ttpload


  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_loca
   *
   * @Description:
   *   Load the locations table.
   *
   * @InOut:
   *   face ::
   *     A handle to the target face object.
   *
   * @Input:
   *   stream ::
   *     The input stream.
   *
   * @Return:
   *   FreeType error code.  0 means success.
   */
  FT_LOCAL_DEF( FT_Error )
  tt_face_load_loca( TT_Face    face,
                     FT_Stream  stream )
  {
    FT_Error  error;
    FT_ULong  table_len;
    FT_Int    shift;

    /* 启动只校验已加载的表目录，不读取 glyf 正文，也不计算整文件哈希。
     * 用减法检查范围，避免坏文件中的 Offset + Length 溢出。 */
    {
      FT_UInt i;
      for ( i = 0; i < face->num_tables; i++ )
      {
        TT_Table table = face->dir_tables + i;
        if ( table->Offset > stream->size ||
             table->Length > stream->size - table->Offset )
          return FT_THROW( Invalid_Table );
      }
    }

    /* we need the size of the `glyf' table for malformed `loca' tables */
    error = face->goto_table( face, TTAG_glyf, stream, &face->glyf_len );

    /* it is possible that a font doesn't have a glyf table at all */
    /* or its size is zero                                         */
    if ( FT_ERR_EQ( error, Table_Missing ) )
    {
      face->glyf_len    = 0;
      face->glyf_offset = 0;
    }
    else if ( error )
      goto Exit;
    else
    {
#ifdef FT_CONFIG_OPTION_INCREMENTAL
      if ( face->root.internal->incremental_interface )
        face->glyf_offset = 0;
      else
#endif
        face->glyf_offset = FT_STREAM_POS();
    }

    FT_TRACE2(( "Locations " ));
    error = face->goto_table( face, TTAG_loca, stream, &table_len );
    if ( error )
    {
      error = FT_THROW( Locations_Missing );
      goto Exit;
    }

    shift = face->header.Index_To_Loc_Format != 0 ? 2 : 1;

    /* 严格拒绝损坏的索引；原有兼容修正会掩盖缺失项，随后在 GUI
     * 自动 hinting 中反复读取 glyf 尾部，最终触发看门狗。 */
    if ( ( face->header.Index_To_Loc_Format != 0 &&
           face->header.Index_To_Loc_Format != 1 ) ||
         table_len != ( (FT_ULong)face->root.num_glyphs + 1 ) << shift )
      return FT_THROW( Invalid_Table );

    if ( table_len > 0x10000UL << shift )
    {
      FT_TRACE2(( "table too large\n" ));
      table_len = 0x10000UL << shift;
    }

    face->num_locations = table_len >> shift;

    if ( face->num_locations != (FT_ULong)face->root.num_glyphs + 1 )
    {
      FT_TRACE2(( "glyph count mismatch!  loca: %ld, maxp: %ld\n",
                  face->num_locations - 1, face->root.num_glyphs ));

      /* we only handle the case where `maxp' gives a larger value */
      if ( face->num_locations < (FT_ULong)face->root.num_glyphs + 1 )
      {
        FT_ULong  new_loca_len =
                    ( (FT_ULong)face->root.num_glyphs + 1 ) << shift;

        TT_Table  entry = face->dir_tables;
        TT_Table  limit = entry + face->num_tables;

        FT_Long  pos   = (FT_Long)FT_STREAM_POS();
        FT_Long  dist  = 0x7FFFFFFFL;
        FT_Bool  found = 0;


        /* compute the distance to next table in font file */
        for ( ; entry < limit; entry++ )
        {
          FT_Long  diff = (FT_Long)entry->Offset - pos;


          if ( diff > 0 && diff < dist )
          {
            dist  = diff;
            found = 1;
          }
        }

        if ( !found )
        {
          /* `loca' is the last table */
          dist = (FT_Long)stream->size - pos;
        }

        if ( new_loca_len <= (FT_ULong)dist )
        {
          face->num_locations = (FT_ULong)face->root.num_glyphs + 1;
          table_len           = new_loca_len;

          FT_TRACE2(( "adjusting num_locations to %ld\n",
                      face->num_locations ));
        }
        else
        {
          face->root.num_glyphs = face->num_locations
                                    ? (FT_Long)face->num_locations - 1 : 0;

          FT_TRACE2(( "adjusting num_glyphs to %ld\n",
                      face->root.num_glyphs ));
        }
      }
    }

    /*
     * Extract the frame.  We don't need to decompress it since
     * we are able to parse it directly.
     */
    if ( FT_FRAME_EXTRACT( table_len, face->glyph_locations ) )
      goto Exit;

    /* FreeType 本来就会读取 loca。遍历这份内存索引，无额外 TF I/O。
     * 相邻偏移允许相等（空字形），但不允许倒序、越界或巨型字形。 */
    {
      FT_Byte* p        = face->glyph_locations;
      FT_ULong previous = 0;
      FT_ULong i;
      for ( i = 0; i < face->num_locations; i++ )
      {
        FT_ULong position = shift == 2 ? FT_NEXT_ULONG( p )
                                       : (FT_ULong)FT_NEXT_USHORT( p ) << 1;
        if ( position < previous || position > face->glyf_len ||
             position - previous > TT_MAX_SAFE_GLYPH_BYTES )
        {
          error = FT_THROW( Invalid_Table );
          goto Exit;
        }
        previous = position;
      }
    }

    FT_TRACE2(( "loaded\n" ));

  Exit:
    return error;
  }


  FT_LOCAL_DEF( FT_ULong )
  tt_face_get_location( FT_Face    face,   /* TT_Face */
                        FT_UInt    gindex,
                        FT_ULong  *asize )
  {
    TT_Face   ttface = (TT_Face)face;
    FT_ULong  pos1, pos2;
    FT_Byte*  p;
    FT_Byte*  p_limit;


    pos1 = pos2 = 0;

    if ( gindex < ttface->num_locations )
    {
      if ( ttface->header.Index_To_Loc_Format != 0 )
      {
        p       = ttface->glyph_locations + gindex * 4;
        p_limit = ttface->glyph_locations + ttface->num_locations * 4;

        pos1 = FT_NEXT_ULONG( p );
        pos2 = pos1;

        if ( p + 4 <= p_limit )
          pos2 = FT_NEXT_ULONG( p );
      }
      else
      {
        p       = ttface->glyph_locations + gindex * 2;
        p_limit = ttface->glyph_locations + ttface->num_locations * 2;

        pos1 = FT_NEXT_USHORT( p );
        pos2 = pos1;

        if ( p + 2 <= p_limit )
          pos2 = FT_NEXT_USHORT( p );

        pos1 <<= 1;
        pos2 <<= 1;
      }
    }

    /* Check broken location data. */
    if ( pos1 > ttface->glyf_len )
    {
      FT_TRACE1(( "tt_face_get_location:"
                  " too large offset (0x%08lx) found for glyph index %d,\n",
                  pos1, gindex ));
      FT_TRACE1(( "                     "
                  " exceeding the end of `glyf' table (0x%08lx)\n",
                  ttface->glyf_len ));
      *asize = TT_MAX_SAFE_GLYPH_BYTES + 1;
      return 0;
    }

    if ( pos2 > ttface->glyf_len )
    {
      /* 不再截断最后一个字形；越界也是损坏，须显式失败并停用后端。 */
      *asize = TT_MAX_SAFE_GLYPH_BYTES + 1;
      return 0;
    }

    /* 索引若在打开后损坏，交给字形帧入口返回错误，禁止读取 glyf 尾部。
     * 不能用长度 0：那会把损坏字形误当作合法空白。 */
    if ( pos2 >= pos1 )
      *asize = (FT_ULong)( pos2 - pos1 );
    else
      *asize = TT_MAX_SAFE_GLYPH_BYTES + 1;

    return pos1;
  }


  FT_LOCAL_DEF( void )
  tt_face_done_loca( TT_Face  face )
  {
    FT_Stream  stream = face->root.stream;


    FT_FRAME_RELEASE( face->glyph_locations );
    face->num_locations = 0;
  }



  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_cvt
   *
   * @Description:
   *   Load the control value table into a face object.
   *
   * @InOut:
   *   face ::
   *     A handle to the target face object.
   *
   * @Input:
   *   stream ::
   *     A handle to the input stream.
   *
   * @Return:
   *   FreeType error code.  0 means success.
   */
  FT_LOCAL_DEF( FT_Error )
  tt_face_load_cvt( TT_Face    face,
                    FT_Stream  stream )
  {
#ifdef TT_USE_BYTECODE_INTERPRETER

    FT_Error   error;
    FT_Memory  memory = stream->memory;
    FT_ULong   table_len;


    FT_TRACE2(( "CVT " ));

    error = face->goto_table( face, TTAG_cvt, stream, &table_len );
    if ( error )
    {
      FT_TRACE2(( "is missing\n" ));

      face->cvt_size = 0;
      face->cvt      = NULL;
      error          = FT_Err_Ok;

      goto Exit;
    }

    face->cvt_size = table_len / 2;

    if ( FT_QNEW_ARRAY( face->cvt, face->cvt_size ) )
      goto Exit;

    if ( FT_FRAME_ENTER( face->cvt_size * 2L ) )
      goto Exit;

    {
      FT_Int32*  cur   = face->cvt;
      FT_Int32*  limit = cur + face->cvt_size;


      for ( ; cur < limit; cur++ )
        *cur = FT_GET_SHORT() * 64;
    }

    FT_FRAME_EXIT();
    FT_TRACE2(( "loaded\n" ));

#ifdef TT_CONFIG_OPTION_GX_VAR_SUPPORT
    if ( face->doblend )
      error = tt_face_vary_cvt( face, stream );
#endif

  Exit:
    return error;

#else /* !TT_USE_BYTECODE_INTERPRETER */

    FT_UNUSED( face   );
    FT_UNUSED( stream );

    return FT_Err_Ok;

#endif
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_fpgm
   *
   * @Description:
   *   Load the font program.
   *
   * @InOut:
   *   face ::
   *     A handle to the target face object.
   *
   * @Input:
   *   stream ::
   *     A handle to the input stream.
   *
   * @Return:
   *   FreeType error code.  0 means success.
   */
  FT_LOCAL_DEF( FT_Error )
  tt_face_load_fpgm( TT_Face    face,
                     FT_Stream  stream )
  {
#ifdef TT_USE_BYTECODE_INTERPRETER

    FT_Error  error;
    FT_ULong  table_len;


    FT_TRACE2(( "Font program " ));

    /* The font program is optional */
    error = face->goto_table( face, TTAG_fpgm, stream, &table_len );
    if ( error )
    {
      face->font_program      = NULL;
      face->font_program_size = 0;
      error                   = FT_Err_Ok;

      FT_TRACE2(( "is missing\n" ));
    }
    else
    {
      face->font_program_size = table_len;
      if ( FT_FRAME_EXTRACT( table_len, face->font_program ) )
        goto Exit;

      FT_TRACE2(( "loaded, %12ld bytes\n", face->font_program_size ));
    }

  Exit:
    return error;

#else /* !TT_USE_BYTECODE_INTERPRETER */

    FT_UNUSED( face   );
    FT_UNUSED( stream );

    return FT_Err_Ok;

#endif
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_prep
   *
   * @Description:
   *   Load the cvt program.
   *
   * @InOut:
   *   face ::
   *     A handle to the target face object.
   *
   * @Input:
   *   stream ::
   *     A handle to the input stream.
   *
   * @Return:
   *   FreeType error code.  0 means success.
   */
  FT_LOCAL_DEF( FT_Error )
  tt_face_load_prep( TT_Face    face,
                     FT_Stream  stream )
  {
#ifdef TT_USE_BYTECODE_INTERPRETER

    FT_Error  error;
    FT_ULong  table_len;


    FT_TRACE2(( "Prep program " ));

    error = face->goto_table( face, TTAG_prep, stream, &table_len );
    if ( error )
    {
      face->cvt_program      = NULL;
      face->cvt_program_size = 0;
      error                  = FT_Err_Ok;

      FT_TRACE2(( "is missing\n" ));
    }
    else
    {
      face->cvt_program_size = table_len;
      if ( FT_FRAME_EXTRACT( table_len, face->cvt_program ) )
        goto Exit;

      FT_TRACE2(( "loaded, %12ld bytes\n", face->cvt_program_size ));
    }

  Exit:
    return error;

#else /* !TT_USE_BYTECODE_INTERPRETER */

    FT_UNUSED( face   );
    FT_UNUSED( stream );

    return FT_Err_Ok;

#endif
  }


  FT_COMPARE_DEF( int )
  compare_ppem( const void*  a,
                const void*  b )
  {
    return **(FT_Byte**)a - **(FT_Byte**)b;
  }


  /**************************************************************************
   *
   * @Function:
   *   tt_face_load_hdmx
   *
   * @Description:
   *   Load the `hdmx' table into the face object.
   *
   * @Input:
   *   face ::
   *     A handle to the target face object.
   *
   *   stream ::
   *     A handle to the input stream.
   *
   * @Return:
   *   FreeType error code.  0 means success.
   */

  FT_LOCAL_DEF( FT_Error )
  tt_face_load_hdmx( TT_Face    face,
                     FT_Stream  stream )
  {
    FT_Error   error;
    FT_Memory  memory = stream->memory;
    FT_UInt    nn, num_records;
    FT_ULong   table_size, record_size;
    FT_Byte*   p;
    FT_Byte*   limit;


    /* this table is optional */
    error = face->goto_table( face, TTAG_hdmx, stream, &table_size );
    if ( error || table_size < 8 )
      return FT_Err_Ok;

    if ( FT_FRAME_EXTRACT( table_size, face->hdmx_table ) )
      goto Exit;

    p     = face->hdmx_table;
    limit = p + table_size;

    /* Given that `hdmx' tables are losing its importance (for example, */
    /* variation fonts introduced in OpenType 1.8 must not have this    */
    /* table) we no longer test for a correct `version' field.          */
    p          += 2;
    num_records = FT_NEXT_USHORT( p );
    record_size = FT_NEXT_ULONG( p );

    /* There are at least two fonts, HANNOM-A and HANNOM-B version */
    /* 2.0 (2005), which get this wrong: The upper two bytes of    */
    /* the size value are set to 0xFF instead of 0x00.  We catch   */
    /* and fix this.                                               */

    if ( record_size >= 0xFFFF0000UL )
      record_size &= 0xFFFFU;

    FT_TRACE2(( "Hdmx " ));

    /* The limit for `num_records' is a heuristic value. */
    if ( num_records > 255 || num_records == 0 )
    {
      FT_TRACE2(( "with unreasonable %u records rejected\n", num_records ));
      goto Fail;
    }

    /* Out-of-spec tables are rejected.  The record size must be */
    /* equal to the number of glyphs + 2 + 32-bit padding.       */
    if ( (FT_Long)record_size != ( ( face->root.num_glyphs + 2 + 3 ) & ~3 ) )
    {
      FT_TRACE2(( "with record size off by %ld bytes rejected\n",
                  (FT_Long)record_size -
                    ( ( face->root.num_glyphs + 2 + 3 ) & ~3 ) ));
      goto Fail;
    }

    if ( FT_QNEW_ARRAY( face->hdmx_records, num_records ) )
      goto Fail;

    for ( nn = 0; nn < num_records; nn++ )
    {
      if ( p + record_size > limit )
        break;
      face->hdmx_records[nn] = p;
      p                     += record_size;
    }

    /* The records must be already sorted by ppem but it does not */
    /* hurt to make sure so that the binary search works later.   */
    ft_qsort( face->hdmx_records, nn, sizeof ( FT_Byte* ), compare_ppem );

    face->hdmx_record_count = nn;
    face->hdmx_table_size   = table_size;
    face->hdmx_record_size  = record_size;

    FT_TRACE2(( "%ux%lu loaded\n", num_records, record_size ));

  Exit:
    return error;

  Fail:
    FT_FRAME_RELEASE( face->hdmx_table );
    face->hdmx_table_size = 0;
    goto Exit;
  }


  FT_LOCAL_DEF( void )
  tt_face_free_hdmx( TT_Face  face )
  {
    FT_Stream  stream = face->root.stream;
    FT_Memory  memory = stream->memory;


    FT_FREE( face->hdmx_records );
    FT_FRAME_RELEASE( face->hdmx_table );
  }


  /**************************************************************************
   *
   * Return the advance width table for a given pixel size if it is found
   * in the font's `hdmx' table (if any).  The records must be sorted for
   * the binary search to work properly.
   */
  FT_LOCAL_DEF( FT_Byte* )
  tt_face_get_device_metrics( TT_Face  face,
                              FT_UInt  ppem,
                              FT_UInt  gindex )
  {
    FT_UInt   min    = 0;
    FT_UInt   max    = face->hdmx_record_count;
    FT_UInt   mid;
    FT_Byte*  result = NULL;


    while ( min < max )
    {
      mid = ( min + max ) >> 1;

      if ( face->hdmx_records[mid][0] > ppem )
        max = mid;
      else if ( face->hdmx_records[mid][0] < ppem )
        min = mid + 1;
      else
      {
        result = face->hdmx_records[mid] + 2 + gindex;
        break;
      }
    }

    return result;
  }


/* END */
