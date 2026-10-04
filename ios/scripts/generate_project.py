#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Deterministic Xcode project generator; Python standard library only."""
from pathlib import Path
import hashlib
import json
import plistlib

ROOT = Path(__file__).resolve().parents[1]
objects = {}
def ident(name):
    return hashlib.sha256(name.encode()).hexdigest()[:24].upper()
def add(name, value):
    key = ident(name); objects[key] = value; return key

def emit(value, depth=0):
    indent = '\t' * depth
    if isinstance(value, dict):
        return '{\n' + ''.join(f'{indent}\t{json.dumps(str(k))} = {emit(v,depth+1)};\n' for k,v in value.items()) + indent + '}'
    if isinstance(value, list):
        return '(\n' + ''.join(f'{indent}\t{emit(v,depth+1)},\n' for v in value) + indent + ')'
    if isinstance(value, int): return str(value)
    return json.dumps(str(value))

sources = sorted([str(p.relative_to(ROOT)) for p in (ROOT/'App').glob('*.swift')]
                 + [str(p.relative_to(ROOT)) for p in (ROOT/'Core').glob('*.swift')]
                 + ['App/Viewer.metal', 'Native/NDNative.c', '../src/crypto.c'])
refs=[]; source_build=[]; resource_build=[]; framework_build=[]
for path in sources + ['App/Info.plist','App/PrivacyInfo.xcprivacy','Native/NDNative.h','Native/NetDisplay-Bridging-Header.h','App/Assets.xcassets']:
    types={'.swift':'sourcecode.swift','.metal':'sourcecode.metal','.c':'sourcecode.c.c','.h':'sourcecode.c.h','.plist':'text.plist.xml','.xcprivacy':'text.plist.xml','.xcassets':'folder.assetcatalog'}
    file=add('file:'+path,{'isa':'PBXFileReference','lastKnownFileType':types[Path(path).suffix],'path':path,'sourceTree':'SOURCE_ROOT'})
    refs.append(file)
    if path in sources:
        source_build.append(add('build:'+path,{'isa':'PBXBuildFile','fileRef':file}))
    elif path.endswith(('.xcprivacy','.xcassets')):
        resource_build.append(add('build:'+path,{'isa':'PBXBuildFile','fileRef':file}))
for name in ['UIKit','SwiftUI','Foundation','Security','ARKit','AVFoundation','CoreMotion','CoreMedia','CoreVideo','VideoToolbox','Metal','MetalKit','QuartzCore']:
    path=f'System/Library/Frameworks/{name}.framework'
    file=add('framework:'+name,{'isa':'PBXFileReference','lastKnownFileType':'wrapper.framework','name':name+'.framework','path':path,'sourceTree':'SDKROOT'})
    refs.append(file); framework_build.append(add('link:'+name,{'isa':'PBXBuildFile','fileRef':file}))
sodium=add('sodium',{'isa':'PBXFileReference','lastKnownFileType':'archive.ar','name':'libsodium.a','path':'build/sodium/iphoneos-arm64/lib/libsodium.a','sourceTree':'SOURCE_ROOT'})
refs.append(sodium); framework_build.append(add('link:sodium',{'isa':'PBXBuildFile','fileRef':sodium}))
product=add('product',{'isa':'PBXFileReference','explicitFileType':'wrapper.application','includeInIndex':0,'path':'NetDisplay.app','sourceTree':'BUILT_PRODUCTS_DIR'})
product_group=add('products',{'isa':'PBXGroup','children':[product],'name':'Products','sourceTree':'<group>'})
main_group=add('main-group',{'isa':'PBXGroup','children':refs+[product_group],'sourceTree':'<group>'})
phases=[]
for name,kind,files in [('sources','PBXSourcesBuildPhase',source_build),('frameworks','PBXFrameworksBuildPhase',framework_build),('resources','PBXResourcesBuildPhase',resource_build)]:
    phases.append(add(name,{'isa':kind,'buildActionMask':2147483647,'files':files,'runOnlyForDeploymentPostprocessing':0}))
project_configs=[]; target_configs=[]
for config in ['Debug','Release']:
    project_settings={
        'ALWAYS_SEARCH_USER_PATHS':'NO','CLANG_ENABLE_MODULES':'YES','CLANG_ENABLE_OBJC_ARC':'YES',
        'CLANG_WARN_DOCUMENTATION_COMMENTS':'YES','CLANG_WARN_EMPTY_BODY':'YES','CLANG_WARN_INFINITE_RECURSION':'YES',
        'GCC_C_LANGUAGE_STANDARD':'gnu11','GCC_WARN_64_TO_32_BIT_CONVERSION':'YES','GCC_WARN_UNDECLARED_SELECTOR':'YES',
        'GCC_WARN_UNINITIALIZED_AUTOS':'YES_AGGRESSIVE','IPHONEOS_DEPLOYMENT_TARGET':'17.0','SDKROOT':'iphoneos',
        'SWIFT_VERSION':'5.0','SWIFT_STRICT_CONCURRENCY':'minimal','ENABLE_USER_SCRIPT_SANDBOXING':'YES',
        'DEBUG_INFORMATION_FORMAT':'dwarf' if config=='Debug' else 'dwarf-with-dsym',
        'GCC_OPTIMIZATION_LEVEL':'0' if config=='Debug' else 's',
        'SWIFT_OPTIMIZATION_LEVEL':'-Onone' if config=='Debug' else '-O',
        'MTL_ENABLE_DEBUG_INFO':'INCLUDE_SOURCE' if config=='Debug' else 'NO',
        'MTL_FAST_MATH':'YES','ENABLE_NS_ASSERTIONS':'YES' if config=='Debug' else 'NO',
    }
    if config=='Debug': project_settings['SWIFT_ACTIVE_COMPILATION_CONDITIONS']='DEBUG'
    project_configs.append(add('project:'+config,{'isa':'XCBuildConfiguration','buildSettings':project_settings,'name':config}))
    target_settings={
        'PRODUCT_NAME':'NetDisplay','PRODUCT_BUNDLE_IDENTIFIER':'io.github.aliemresenel.netdisplay',
        'MARKETING_VERSION':'1.0.0','CURRENT_PROJECT_VERSION':'1',
        'INFOPLIST_FILE':'App/Info.plist','GENERATE_INFOPLIST_FILE':'NO',
        'ASSETCATALOG_COMPILER_APPICON_NAME':'AppIcon','ASSETCATALOG_COMPILER_GENERATE_SWIFT_ASSET_SYMBOL_EXTENSIONS':'NO',
        'CODE_SIGNING_ALLOWED':'NO','CODE_SIGNING_REQUIRED':'NO','CODE_SIGN_IDENTITY':'',
        'TARGETED_DEVICE_FAMILY':'1','SUPPORTED_PLATFORMS':'iphoneos',
        'SUPPORTS_MACCATALYST':'NO','SUPPORTS_MAC_DESIGNED_FOR_IPHONE_IPAD':'NO',
        'ARCHS':'arm64','SWIFT_OBJC_BRIDGING_HEADER':'Native/NetDisplay-Bridging-Header.h',
        'HEADER_SEARCH_PATHS':['$(inherited)','$(SRCROOT)/../src','$(SRCROOT)/Native','$(SRCROOT)/build/sodium/iphoneos-arm64/include'],
        'LIBRARY_SEARCH_PATHS':['$(inherited)','$(SRCROOT)/build/sodium/iphoneos-arm64/lib'],
        'LD_RUNPATH_SEARCH_PATHS':['$(inherited)','@executable_path/Frameworks'],
        'ALWAYS_EMBED_SWIFT_STANDARD_LIBRARIES':'YES',
        'SWIFT_EMIT_LOC_STRINGS':'NO','ENABLE_BITCODE':'NO',
    }
    target_configs.append(add('target:'+config,{'isa':'XCBuildConfiguration','buildSettings':target_settings,'name':config}))
project_list=add('project-configs',{'isa':'XCConfigurationList','buildConfigurations':project_configs,'defaultConfigurationIsVisible':0,'defaultConfigurationName':'Release'})
target_list=add('target-configs',{'isa':'XCConfigurationList','buildConfigurations':target_configs,'defaultConfigurationIsVisible':0,'defaultConfigurationName':'Release'})
target=add('target',{'isa':'PBXNativeTarget','buildConfigurationList':target_list,'buildPhases':phases,'buildRules':[],'dependencies':[],'name':'NetDisplay','productName':'NetDisplay','productReference':product,'productType':'com.apple.product-type.application'})
project=add('project',{'isa':'PBXProject','attributes':{'LastUpgradeCheck':'1600','LastSwiftUpdateCheck':'1600','BuildIndependentTargetsInParallel':'YES','TargetAttributes':{target:{'CreatedOnToolsVersion':'16.0'}}},'buildConfigurationList':project_list,'compatibilityVersion':'Xcode 14.0','developmentRegion':'en','hasScannedForEncodings':0,'knownRegions':['en','Base'],'mainGroup':main_group,'productRefGroup':product_group,'projectDirPath':'','projectRoot':'','targets':[target]})
pbx={'archiveVersion':1,'classes':{},'objectVersion':56,'objects':objects,'rootObject':project}
project_dir=ROOT/'NetDisplay.xcodeproj'; project_dir.mkdir(exist_ok=True)
(project_dir/'project.pbxproj').write_text('// !$*UTF8*$!\n'+emit(pbx)+'\n')
scheme_dir=project_dir/'xcshareddata'/'xcschemes'; scheme_dir.mkdir(parents=True,exist_ok=True)
ref=f'<BuildableReference BuildableIdentifier="primary" BlueprintIdentifier="{target}" BuildableName="NetDisplay.app" BlueprintName="NetDisplay" ReferencedContainer="container:NetDisplay.xcodeproj"/>'
(scheme_dir/'NetDisplay.xcscheme').write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion="1600" version="1.7">
 <BuildAction parallelizeBuildables="YES" buildImplicitDependencies="YES"><BuildActionEntries><BuildActionEntry buildForTesting="YES" buildForRunning="YES" buildForProfiling="YES" buildForArchiving="YES" buildForAnalyzing="YES">{ref}</BuildActionEntry></BuildActionEntries></BuildAction>
 <LaunchAction buildConfiguration="Debug" selectedDebuggerIdentifier="Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier="Xcode.IDEFoundation.Launcher.LLDB" launchStyle="0" useCustomWorkingDirectory="NO" ignoresPersistentStateOnLaunch="NO" debugDocumentVersioning="YES" debugServiceExtension="internal" allowLocationSimulation="YES"><BuildableProductRunnable runnableDebuggingMode="0">{ref}</BuildableProductRunnable></LaunchAction>
 <ProfileAction buildConfiguration="Release" shouldUseLaunchSchemeArgsEnv="YES" savedToolIdentifier="" useCustomWorkingDirectory="NO" debugDocumentVersioning="YES"><BuildableProductRunnable runnableDebuggingMode="0">{ref}</BuildableProductRunnable></ProfileAction>
 <AnalyzeAction buildConfiguration="Debug"/>
 <ArchiveAction buildConfiguration="Release" revealArchiveInOrganizer="YES"/>
</Scheme>
''')
print(f'Generated {project_dir} with {len(sources)} source files')
