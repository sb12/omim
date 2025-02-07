package com.mapswithme.util;

import android.support.annotation.IntDef;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

public class UTM
{
  @Retention(RetentionPolicy.SOURCE)
  @IntDef({ UTM_NONE, UTM_BOOKMARKS_PAGE_CATALOG_BUTTON, UTM_TOOLBAR_BUTTON,
            UTM_DOWNLOAD_MWM_BANNER, UTM_PLACEPAGE_GALLERY, UTM_DISCOVERY_PAGE_GALLERY,
            UTM_TIPS_AND_TRICKS, UTM_BOOKING_PROMO })
  public @interface UTMType {}

  // The order of these constants must correspond to C++ enumeration in partners_api/utm.hpp.
  public static final int UTM_NONE = 0;
  public static final int UTM_BOOKMARKS_PAGE_CATALOG_BUTTON = 1;
  public static final int UTM_TOOLBAR_BUTTON = 2;
  public static final int UTM_DOWNLOAD_MWM_BANNER = 3;
  public static final int UTM_PLACEPAGE_GALLERY = 4;
  public static final int UTM_DISCOVERY_PAGE_GALLERY = 5;
  public static final int UTM_TIPS_AND_TRICKS = 6;
  public static final int UTM_BOOKING_PROMO = 7;
}
