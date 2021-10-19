package org.intellij.sdk.language.psi;

public enum SdDeclarationType {
    DOCUMENT("Document"),
    STRUCT("Struct"),
    SCHEMA_FIELD("Field (in Schema)"),
    DOCUMENT_FIELD("Field (in Document)"),
    STRUCT_FIELD("Struct-Field"),
    DOCUMENT_STRUCT_FIELD("Field (in Struct)"),
    IMPORTED_FIELD("Imported Field"),
    DOCUMENT_SUMMARY("Document-Summary"),
    RANK_PROFILE("Rank Profile"),
    MACRO("Macro"),
    MACRO_ARGUMENT("Macro's Argument"),
    QUERY("Query (first use in file)"),
    ITEM_RAW_SCORE("ItemRawScore (first use in file)");
    
    private final String typeName;
    SdDeclarationType(String name) {
        this.typeName = name;
    }
    
    @Override
    public String toString() {
        return typeName;
    }
}