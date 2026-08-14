#include "AqueductShader.h"

IMPLEMENT_GLOBAL_SHADER(FAqueductTestCS, "/Plugin/Aqueduct/Private/AqueductCompute.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductBlitCS, "/Plugin/Aqueduct/Private/AqueductBlit.usf", "BlitCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductDitherCS, "/Plugin/Aqueduct/Private/AqueductDither.usf", "DitherCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductMipGenCS, "/Plugin/Aqueduct/Private/AqueductMipGen.usf", "MipGenCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductMipGenR32CS, "/Plugin/Aqueduct/Private/AqueductMipGen.usf", "MipGenR32CS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductPackColorCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "PackColorCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductPackOrmCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "PackOrmCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductPackNormalCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "PackNormalCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductPackGrayCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "PackGrayCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductDownsampleCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "DownsampleCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductDownsampleGrayCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "DownsampleGrayCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductCompositeOrmRoughnessCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "CompositeOrmRoughnessCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductCompositeBlurHCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "CompositeBlurHCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductCompositeBlurVCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "CompositeBlurVCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductCompositeChainDownCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "CompositeChainDownCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FAqueductNormalizeNormalCS, "/Plugin/Aqueduct/Private/AqueductDeliver.usf", "NormalizeNormalCS", SF_Compute);
