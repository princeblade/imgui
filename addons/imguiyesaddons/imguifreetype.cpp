/*
//- Common Code For All Addons needed just to ease inclusion as separate files in user code ----------------------
#include <imgui.h>
#undef IMGUI_DEFINE_PLACEMENT_NEW
#define IMGUI_DEFINE_PLACEMENT_NEW
#undef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_internal.h>
//-----------------------------------------------------------------------------------------------------------------
*/

#include "imguifreetype.h"

// Original repository: https://github.com/Vuhdo/imgui_freetype
// MIT licensed

#include <stdint.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H


/*#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_PLACEMENT_NEW
#include "imgui_internal.h"*/

#define STBRP_ASSERT(x)    IM_ASSERT(x)
//#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"


namespace ImGuiFreeType {

#ifdef IMGUI_USE_AUTO_BINDING
ImU32 DefaultRasterizationFlags = 0;
ImVector<ImU32> DefaultRasterizationFlagVector;
#endif //IMGUI_USE_AUTO_BINDING

/// Font parameters and metrics.
struct FontInfo {
    /// Size this font was generated with.
    uint32_t pixelHeight;

    /// The pixel extents above the baseline in pixels (typically positive).
    float ascender;
    /// The extents below the baseline in pixels (typically negative).
    float descender;

    /// The baseline-to-baseline distance. Note that it usually is larger than the
    /// sum of the ascender and descender taken as absolute values. There is also no
    /// guarantee that no glyphs extend above or below subsequent baselines when
    /// using this distance  think of it as a value the designer of the font finds appropriate.
    float lineSpacing;

    /// The spacing in pixels between one row's descent and the next row's ascent.
    float lineGap;

    /// This field gives the maximum horizontal cursor advance for all glyphs in the font.
    float maxAdvanceWidth;

    /// The number of glyphs available in the font face.
    uint32_t glyphsCount;

    //
    const char* familyName;
    const char* styleName;
};

/// A structure that describe a glyph.
struct GlyphInfo {
    float width;		/// Glyph's width in pixels.
    float height;		/// Glyph's height in pixels.
    float offsetX;		/// The distance from the origin ("pen position") to the left of the glyph.
    float offsetY;		/// The distance from the origin to the top of the glyph. This is usually a value < 0.
    float advanceX;		/// The distance from the origin to the origin of the next glyph. This is usually a value > 0.
};


/// Rasterized glyph image (8-bit alpha coverage).
struct GlyphBitmap {
    static const uint32_t MaxWidth = 256;
    static const uint32_t MaxHeight = 256;
    uint8_t  grayscale[ MaxWidth * MaxHeight ];
    uint32_t width, height, pitch;
};


//
template< class OutputType, class FreeTypeFixed > inline static
OutputType Round26Dot6( FreeTypeFixed v ) {
    //return ( OutputType )round( v / 64.0f );
    return (OutputType) (((v + 63) & -64) / 64);
//  From SDL_ttf: Handy routines for converting from fixed point
//  #define FT_CEIL(X)  (((X + 63) & -64) / 64)
}

//
//  FreeType glyph rasterizer.
//
class FreeTypeFont {
public:
    /// no ctor/dtor, explicitly call Init()/Shutdown()

    /// Font descriptor of the current font.
    FontInfo fontInfo;

    /// Initialize from an external data buffer.
    /// Doesn't copy data, and you must ensure it stays valid up to this object lifetime.
    void Init( const uint8_t* data, uint32_t dataSize, uint32_t faceIndex, uint32_t pixelHeight ) {
        // TODO: substitute allocator
        FT_Error error = FT_Init_FreeType( &m_library );
        IM_ASSERT( error == 0 );
        error = FT_New_Memory_Face( m_library, data, dataSize, faceIndex, &m_face );
        IM_ASSERT( error == 0 );
        error = FT_Select_Charmap( m_face, FT_ENCODING_UNICODE );
        IM_ASSERT( error == 0 );

        memset( &fontInfo, 0, sizeof( fontInfo ) );
        SetPixelHeight( pixelHeight );

        // fill up the font info
        //FT_Size_Metrics metrics = m_face->size->metrics;
        fontInfo.pixelHeight = pixelHeight;
        fontInfo.glyphsCount = m_face->num_glyphs;
        fontInfo.familyName = m_face->family_name;
        fontInfo.styleName = m_face->style_name;
    }
    /// Cleanup.
    void Shutdown() {
        if( m_face ) {
            FT_Done_Face( m_face );
            m_face = NULL;
            FT_Done_FreeType( m_library );
            m_library = NULL;
        }
    }
    /// Change font pixel size. All following calls to RasterizeGlyph() will use this size.
    void SetPixelHeight( uint32_t pixelHeight ) {
        // I'm not sure how to deal with font sizes properly.
        // As far as I understand, currently ImGui assumes that the 'pixelHeight' is a maximum height of an any given glyph,
        // i.e. it's the sum of font's ascender and descender.
        // Seems strange to me.

        FT_Size_RequestRec req;
        req.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
        req.width = 0;
        req.height = pixelHeight * 64;
        req.horiResolution = 0;
        req.vertResolution = 0;
        FT_Request_Size( m_face, &req );

        // update font info
        FT_Size_Metrics metrics = m_face->size->metrics;
        fontInfo.pixelHeight = pixelHeight;
        fontInfo.ascender = Round26Dot6< float >( metrics.ascender );
        fontInfo.descender = Round26Dot6< float >( metrics.descender );
        fontInfo.lineSpacing = Round26Dot6< float >( metrics.height );
        fontInfo.lineGap = Round26Dot6< float >( metrics.height - metrics.ascender + metrics.descender );
        fontInfo.maxAdvanceWidth = Round26Dot6< float >( metrics.max_advance );
    }
    /// Generate glyph image.
    bool RasterizeGlyph( uint32_t codepoint, GlyphInfo& glyphInfo, GlyphBitmap& glyphBitmap, uint32_t flags ) {


        // load the glyph we are looking for
	FT_Int32 LoadFlags = FT_LOAD_NO_BITMAP;	// | FT_LOAD_COLOR;
        if( flags & ImGuiFreeType::DisableHinting )
            LoadFlags |= FT_LOAD_NO_HINTING;
        if( flags & ImGuiFreeType::ForceAutoHint )
            LoadFlags |= FT_LOAD_FORCE_AUTOHINT;
        if( flags & ImGuiFreeType::NoAutoHint )
            LoadFlags |= FT_LOAD_NO_AUTOHINT;

        if( flags & ImGuiFreeType::LightHinting )
            LoadFlags |= FT_LOAD_TARGET_LIGHT;
        else if( flags & ImGuiFreeType::MonoHinting )
            LoadFlags |= FT_LOAD_TARGET_MONO;
        else
	    LoadFlags |= FT_LOAD_TARGET_NORMAL;

        uint32_t glyphIndex = FT_Get_Char_Index( m_face, codepoint );
        FT_Error error = FT_Load_Glyph( m_face, glyphIndex, LoadFlags );
        if( error )
            return false;

        FT_GlyphSlot slot = m_face->glyph;	// shortcut

        // need an outline for this to work
        IM_ASSERT( slot->format == FT_GLYPH_FORMAT_OUTLINE );

        if( flags & ImGuiFreeType::Oblique )
            FT_GlyphSlot_Oblique( slot );

        if( flags & ImGuiFreeType::Bold )
            FT_GlyphSlot_Embolden( slot );

        // retrieve the glyph
        FT_Glyph glyphDesc;
	error = FT_Get_Glyph( slot, &glyphDesc );


	if( error != 0 )
            return false;

        // rasterize
	error = FT_Glyph_To_Bitmap( &glyphDesc,
				    FT_RENDER_MODE_NORMAL,
				     0, 1 );

        if( error != 0 )
            return false;
        FT_BitmapGlyph freeTypeBitmap = ( FT_BitmapGlyph )glyphDesc;

        //
        glyphInfo.advanceX = Round26Dot6< float >( slot->advance.x );
        glyphInfo.offsetX = ( float )freeTypeBitmap->left;
        glyphInfo.offsetY = -( float )freeTypeBitmap->top;
        glyphInfo.width = ( float )freeTypeBitmap->bitmap.width;
        glyphInfo.height = ( float )freeTypeBitmap->bitmap.rows;
        //
        glyphBitmap.width = freeTypeBitmap->bitmap.width;
        glyphBitmap.height = freeTypeBitmap->bitmap.rows;
        glyphBitmap.pitch = ( uint32_t )freeTypeBitmap->bitmap.pitch;

        IM_ASSERT( glyphBitmap.pitch <= GlyphBitmap::MaxWidth );
	if( glyphBitmap.width > 0 ) {
	    memcpy( glyphBitmap.grayscale, freeTypeBitmap->bitmap.buffer, glyphBitmap.pitch * glyphBitmap.height );
	}

        // cleanup
        FT_Done_Glyph( glyphDesc );

        return true;
    }
private:
    //
    FT_Library m_library;
    FT_Face    m_face;
};


bool BuildFontAtlas( ImFontAtlas* atlas, ImU32 extra_flags=0,const ImVector<ImU32>* pOptionalFlagVector=NULL) {
    using namespace ImGui;
    IM_ASSERT( atlas->ConfigData.Size > 0 );
    IM_ASSERT(atlas->TexGlyphPadding == 1); // Not supported

    ImFontAtlasBuildRegisterDefaultCustomRects(atlas);

    atlas->TexID = NULL;
    atlas->TexWidth = atlas->TexHeight = 0;
    atlas->TexUvWhitePixel = ImVec2( 0, 0 );
    atlas->ClearTexData();

    FreeTypeFont* tmp_array = ( FreeTypeFont* )ImGui::MemAlloc( ( size_t )atlas->ConfigData.Size * sizeof( FreeTypeFont ) );

    ImVec2 max_glyph_size(1.0f, 1.0f);

    // Initialize font information early (so we can error without any cleanup) + count glyphs
    int total_glyphs_count = 0;
    int total_ranges_count = 0;
    for( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ ) {
        ImFontConfig& cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont& fontFace = tmp_array[ input_i ];

        IM_ASSERT( cfg.DstFont && ( !cfg.DstFont->IsLoaded() || cfg.DstFont->ContainerAtlas == atlas ) );

        fontFace.Init( ( uint8_t* )cfg.FontData, ( uint32_t )cfg.FontDataSize, cfg.FontNo, ( uint32_t )cfg.SizePixels );
        max_glyph_size.x = ImMax( max_glyph_size.x, fontFace.fontInfo.maxAdvanceWidth );
        max_glyph_size.y = ImMax( max_glyph_size.y, fontFace.fontInfo.ascender - fontFace.fontInfo.descender );

        // Count glyphs
        if( !cfg.GlyphRanges )
            cfg.GlyphRanges = atlas->GetGlyphRangesDefault();
        for (const ImWchar* in_range = cfg.GlyphRanges; in_range[0] && in_range[ 1 ]; in_range += 2, total_ranges_count++)
            total_glyphs_count += (in_range[1] - in_range[0]) + 1;
    }

    // We need a width for the skyline algorithm. Using a dumb heuristic here to decide of width. User can override TexDesiredWidth and TexGlyphPadding if they wish.
    // Width doesn't really matter much, but some API/GPU have texture size limitations and increasing width can decrease height.
    atlas->TexWidth = (atlas->TexDesiredWidth > 0) ? atlas->TexDesiredWidth : (total_glyphs_count > 4000) ? 4096 : (total_glyphs_count > 2000) ? 2048 : (total_glyphs_count > 1000) ? 1024 : 512;

    // (Vuhdo: Now, I won't do the original first pass to determine texture height, but just rough estimate.
    //  Looks ugly inaccurate and excessive, but AFAIK with FreeType we actually need to render glyphs to get exact sizes.
    //  Alternatively, we could just render all glyphs into a big shadow buffer, get their sizes, do the rectangle packing and just copy back from the
    //  shadow buffer to the texture buffer. Will give us an accurate texture height, but eat a lot of temp memory. Probably no one will notice.)
    const int total_rects = total_glyphs_count + atlas->CustomRects.size();
    float min_rects_per_row = ceilf((atlas->TexWidth / (max_glyph_size.x + 1.0f)));
    float min_rects_per_column = ceilf(total_rects / min_rects_per_row);
    atlas->TexHeight = (int)(min_rects_per_column * (max_glyph_size.y + 1.0f));

    // Create texture
    atlas->TexHeight = ImUpperPowerOfTwo(atlas->TexHeight);
    atlas->TexPixelsAlpha8 = (unsigned char*)ImGui::MemAlloc(atlas->TexWidth * atlas->TexHeight);
    memset(atlas->TexPixelsAlpha8, 0, atlas->TexWidth * atlas->TexHeight);

    // Start packing
    stbrp_node* pack_nodes = (stbrp_node*)ImGui::MemAlloc(total_rects * sizeof(stbrp_node));
    stbrp_context context;
    stbrp_init_target(&context, atlas->TexWidth, atlas->TexHeight, pack_nodes, total_rects);

    // Pack our extra data rectangles first, so it will be on the upper-left corner of our texture (UV will have small values).
    ImFontAtlasBuildPackCustomRects(atlas, &context);

    // render characters, setup ImFont and glyphs for runtime
    for( int input_i = 0; input_i < atlas->ConfigData.Size; input_i++ ) {
        ImFontConfig& cfg = atlas->ConfigData[ input_i ];
        FreeTypeFont& font_face = tmp_array[ input_i ];
        ImFont* dst_font = cfg.DstFont;

        const float ascent = font_face.fontInfo.ascender;
        const float descent = font_face.fontInfo.descender;
        ImFontAtlasBuildSetupFont(atlas, dst_font, &cfg, ascent, descent);
        const float off_x = cfg.GlyphOffset.x;
        const float off_y = cfg.GlyphOffset.y + (float)(int)(dst_font->Ascent + 0.5f);

        bool multiply_enabled = (cfg.RasterizerMultiply != 1.0f);
        unsigned char multiply_table[256];
        if (multiply_enabled)   ImFontAtlasBuildMultiplyCalcLookupTable(multiply_table, cfg.RasterizerMultiply);

        dst_font->FallbackGlyph = NULL; // Always clear fallback so FindGlyph can return NULL. It will be set again in BuildLookupTable()
		
        const uint32_t flag = cfg.RasterizerFlags | ((pOptionalFlagVector && pOptionalFlagVector->size()>input_i) ? (*pOptionalFlagVector)[input_i] : extra_flags);
        for( const ImWchar* in_range = cfg.GlyphRanges; in_range[ 0 ] && in_range[ 1 ]; in_range += 2 ) {
            for( uint32_t codepoint = in_range[ 0 ]; codepoint <= in_range[ 1 ]; ++codepoint ) {

                if( cfg.MergeMode && dst_font->FindGlyph( ( unsigned short )codepoint ) )
                    continue;

                GlyphInfo glyphInfo;
                GlyphBitmap glyphBitmap;
                font_face.RasterizeGlyph( codepoint, glyphInfo, glyphBitmap, flag );

                // blit to texture
                stbrp_rect rect;
                rect.w = ( uint16_t )glyphBitmap.width + 1;		// account for texture filtering
                rect.h = ( uint16_t )glyphBitmap.height + 1;
                stbrp_pack_rects( &context, &rect, 1 );
                const uint8_t* src = glyphBitmap.grayscale;
                uint8_t* dst = atlas->TexPixelsAlpha8 + rect.y * atlas->TexWidth + rect.x;
                for( uint32_t yy = 0; yy < glyphBitmap.height; ++yy ) {
                    memcpy( dst, src, glyphBitmap.width );
                    src += glyphBitmap.pitch;
                    dst += atlas->TexWidth;
                }

                if (multiply_enabled)   ImFontAtlasBuildMultiplyRectAlpha8(multiply_table, atlas->TexPixelsAlpha8, rect.x, rect.y, rect.w, rect.h, atlas->TexWidth);

                dst_font->Glyphs.resize(dst_font->Glyphs.Size + 1);
                ImFont::Glyph& glyph = dst_font->Glyphs.back();
                glyph.Codepoint = (ImWchar)codepoint;
                glyph.X0 = glyphInfo.offsetX + off_x;
                glyph.Y0 = glyphInfo.offsetY + off_y;
                glyph.X1 = glyph.X0 + glyphInfo.width;
                glyph.Y1 = glyph.Y0 + glyphInfo.height;
                glyph.U0 = rect.x / (float)atlas->TexWidth;
                glyph.V0 = rect.y / (float)atlas->TexHeight;
                glyph.U1 = (rect.x + glyphInfo.width) / (float)atlas->TexWidth;
                glyph.V1 = (rect.y + glyphInfo.height) / (float)atlas->TexHeight;
                glyph.XAdvance = (glyphInfo.advanceX + cfg.GlyphExtraSpacing.x);  // Bake spacing into XAdvance

                if (cfg.PixelSnapH)
                    glyph.XAdvance = (float)(int)(glyph.XAdvance + 0.5f);
                dst_font->MetricsTotalSurface += (int)((glyph.U1 - glyph.U0) * atlas->TexWidth + 1.99f) * (int)((glyph.V1 - glyph.V0) * atlas->TexHeight + 1.99f); // +1 to account for average padding, +0.99 to round
            }
        }
        cfg.DstFont->BuildLookupTable();
    }

    // Cleanup temporaries
    for( int n = 0; n < atlas->ConfigData.Size; ++n )
        tmp_array[ n ].Shutdown();
    ImGui::MemFree(pack_nodes);
    ImGui::MemFree( tmp_array );

    // Render into our custom data block
    ImFontAtlasBuildRenderDefaultTexData(atlas);

    return true;
}


void GetTexDataAsAlpha8(ImFontAtlas* atlas,unsigned char** out_pixels, int* out_width, int* out_height, int* out_bytes_per_pixel,ImU32 flags,const ImVector<ImU32>* pOptionalFlagVector)   {
    using namespace ImGui;
    // Build atlas on demand
    if (atlas->TexPixelsAlpha8 == NULL)
    {
        if (atlas->ConfigData.empty())
            atlas->AddFontDefault();
        BuildFontAtlas(atlas,flags,pOptionalFlagVector);
    }

    *out_pixels = atlas->TexPixelsAlpha8;
    if (out_width) *out_width = atlas->TexWidth;
    if (out_height) *out_height = atlas->TexHeight;
    if (out_bytes_per_pixel) *out_bytes_per_pixel = 1;
}
void GetTexDataAsRGBA32(ImFontAtlas* atlas,unsigned char** out_pixels, int* out_width, int* out_height, int* out_bytes_per_pixel,ImU32 flags,const ImVector<ImU32>* pOptionalFlagVector)    {
    // Convert to RGBA32 format on demand
    // Although it is likely to be the most commonly used format, our font rendering is 1 channel / 8 bpp
    if (!atlas->TexPixelsRGBA32)
    {
        unsigned char* pixels;
        GetTexDataAsAlpha8(atlas,&pixels, NULL, NULL,NULL,flags,pOptionalFlagVector);
        atlas->TexPixelsRGBA32 = (unsigned int*)ImGui::MemAlloc((size_t)(atlas->TexWidth * atlas->TexHeight * 4));
        const unsigned char* src = pixels;
        unsigned int* dst = atlas->TexPixelsRGBA32;
        for (int n = atlas->TexWidth * atlas->TexHeight; n > 0; n--)
	    *dst++ = IM_COL32(255, 255, 255, (unsigned int)(*src++));
    }

    *out_pixels = (unsigned char*)atlas->TexPixelsRGBA32;
    if (out_width) *out_width = atlas->TexWidth;
    if (out_height) *out_height = atlas->TexHeight;
    if (out_bytes_per_pixel) *out_bytes_per_pixel = 4;
}



} // namespace ImGuiFreeType

