#include <parser.hpp>
#include <fstream>
#include <sstream>
#include <codegen.hpp>

using namespace llvm;
using namespace CodeGenerator;
using namespace std;
namespace TokenType = Enum::Token::Type;
namespace TokenKind = Enum::Token::Kind;

#define SET_PRIMITIVE_OPCODE(method_int, method_double, name, ret) do { \
		if (left_type == Enum::Runtime::Int && right_type == Enum::Runtime::Int) { \
			ret = method_int(left_value, right_value, name);			\
			cur_type = Enum::Runtime::Int;								\
		} else {														\
			llvm::Value *casted_left_value = builder->CreateSIToFP(left_value, llvm::Type::getDoubleTy(module->getContext())); \
			llvm::Value *casted_right_value = builder->CreateSIToFP(right_value, llvm::Type::getDoubleTy(module->getContext())); \
			ret = method_double(casted_left_value, casted_right_value, "f" # name); \
			cur_type = Enum::Runtime::Double;							\
		}																\
	} while (0)

#define SET_COMPARE_OPCODE(method_int, method_double, name, ret) do {	\
		if (left_type == Enum::Runtime::Int && right_type == Enum::Runtime::Int) { \
			ret = builder->CreateZExt(method_int(left_value, right_value, name), llvm::Type::getInt64Ty(module->getContext())); \
			cur_type = Enum::Runtime::Int;								\
		} else {														\
			llvm::Value *casted_left_value = builder->CreateSIToFP(left_value, llvm::Type::getDoubleTy(module->getContext())); \
			llvm::Value *casted_right_value = builder->CreateSIToFP(right_value, llvm::Type::getDoubleTy(module->getContext())); \
			ret = builder->CreateZExt(method_double(casted_left_value, casted_right_value, name), llvm::Type::getInt64Ty(module->getContext())); \
			cur_type = Enum::Runtime::Int;								\
		}																\
	} while (0)


LLVM::LLVM(void)
{
	LLVMContext &ctx = getGlobalContext();
	module = new llvm::Module("LLVMIR", ctx);
	createRuntimeTypes();
	cur_args = NULL;
}

void LLVM::createRuntimeTypes(void)
{
	LLVMContext &ctx = getGlobalContext();
	vector<Type *> fields;
	fields.push_back(Type::getInt64Ty(ctx));
	fields.push_back(Type::getDoubleTy(ctx));
	fields.push_back(Type::getInt8PtrTy(ctx));
	fields.push_back(Type::getInt8PtrTy(ctx));
	value_type = StructType::create(ArrayRef<Type *>(fields), "Value");

	fields.clear();

	value_ptr_type = PointerType::get(value_type, 0);
	fields.push_back(Type::getInt32Ty(ctx));
	fields.push_back(value_type);
	//fields.push_back(value_ptr_type);
	object_type = StructType::create(ArrayRef<Type *>(fields), "Object");

	fields.clear();

	object_ptr_type = PointerType::get(object_type, 0);
	object_double_ptr_type = PointerType::get(object_ptr_type, 0);

	fields.push_back(Type::getInt32Ty(ctx));
	//fields.push_back(object_ptr_type);
	fields.push_back(PointerType::get(object_ptr_type, 0));
	fields.push_back(Type::getInt64Ty(ctx));
	array_type = StructType::create(ArrayRef<Type *>(fields), "ArrayObject");
	array_ptr_type = PointerType::get(array_type, 0);
}

bool LLVM::linkModule(llvm::Module *dest, string file_name)
{
	SMDiagnostic err;
	string err_msg;
	llvm::Module *link_mod = ParseIRFile(file_name, err, getGlobalContext());
	if (!link_mod) return false;
	if (Linker::LinkModules(dest, link_mod, Linker::DestroySource, &err_msg)) {
		//cout << err_msg << endl;
		return false;
	}
	return true;
}

const char *LLVM::gen(AST *ast)
{
	//fprintf(stdout, "gen!!\n");
	LLVMContext &ctx = getGlobalContext();
	IRBuilder<> builder(ctx);
	//Type *int_type = IntegerType::get(module->getContext(), 64);
	//llvm::FunctionType *mainFuncType = llvm::FunctionType::get(int_type, true);
	llvm::FunctionType *mainFuncType = llvm::FunctionType::get(builder.getVoidTy(), true);
	Function *func_main = Function::Create(
		mainFuncType,
		GlobalValue::ExternalLinkage,
		"main", module
	);
	cur_func = func_main;
	cur_func_name = "main";
	func_main->setCallingConv(CallingConv::C);
	BasicBlock *entry = BasicBlock::Create(ctx, "entrypoint", func_main);
	main_entry = entry;
	builder.SetInsertPoint(entry);

	traverse(&builder, ast);

	builder.CreateRetVoid();
	linkModule(module, "gen/runtime_api.lli");
	//write();
	//module->dump();
	AssemblyAnnotationWriter writer;
	ostringstream os;
	raw_os_ostream raw_stream(os);
	module->print(raw_stream, &writer);
	return os.str().c_str();
}

void LLVM::write(void)
{
	AssemblyAnnotationWriter writer;
	ofstream os("llvm.ir");
	raw_os_ostream raw_stream(os);
	module->print(raw_stream, &writer);
}

void LLVM::traverse(IRBuilder<> *builder, AST *ast)
{
	Node *node = ast->root;
	for (; node != NULL; node = node->next) {
		generateCode(builder, node);
	}
}

void LLVM::generateCode(IRBuilder<> *builder, Node *node)
{
	if (TYPE_match(node, FunctionCallNode)) {
		generateFunctionCallCode(builder, dynamic_cast<FunctionCallNode *>(node));
	} else if (TYPE_match(node, LeafNode)) {
		generateValueCode(builder, node);
	} else if (TYPE_match(node, BranchNode)) {
		if (node->tk->info.kind == TokenKind::Assign) {
			generateAssignCode(builder, dynamic_cast<BranchNode *>(node));
		} else {
			generateOperatorCode(builder, dynamic_cast<BranchNode *>(node));
		}
	} else if (TYPE_match(node, IfStmtNode)) {
		generateIfStmtCode(builder, dynamic_cast<IfStmtNode *>(node));
	} else if (TYPE_match(node, ElseStmtNode)) {
		generateElseStmtCode(builder, dynamic_cast<ElseStmtNode *>(node));
	} else if (TYPE_match(node, ForStmtNode)) {
		generateForStmtCode(builder, dynamic_cast<ForStmtNode *>(node));
	} else if (TYPE_match(node, SingleTermOperatorNode)) {
		generateSingleTermOperatorCode(builder, dynamic_cast<SingleTermOperatorNode *>(node));
	} else if (TYPE_match(node, WhileStmtNode)) {
		generateWhileStmtCode(builder, dynamic_cast<WhileStmtNode *>(node));
	} else if (TYPE_match(node, ForeachStmtNode)) {
		generateForeachStmtCode(builder, dynamic_cast<ForeachStmtNode *>(node));
	} else if (TYPE_match(node, FunctionNode)) {
		generateFunctionCode(builder, dynamic_cast<FunctionNode *>(node));
	} else if (TYPE_match(node, ReturnNode)) {
		generateReturnCode(builder, dynamic_cast<ReturnNode *>(node));
	} else {
		asm("int3");
	}
}

BasicBlock *LLVM::generateBlockCode(IRBuilder<> *builder, BasicBlock *block, BasicBlock *merge_block, Node *node)
{
	builder->SetInsertPoint(block);
	for (; node != NULL; node = node->next) {
		generateCode(builder, node);
	}
	builder->CreateBr(merge_block);
	return block;
}

void LLVM::generateIfStmtCode(IRBuilder<> *builder, IfStmtNode *node)
{
	llvm::Value *expr = generateValueCode(builder, node->expr);
	llvm::Value *zero = ConstantInt::get(IntegerType::get(module->getContext(), 64), 0);
	llvm::Value *cond = builder->CreateICmpNE(expr, zero);
	LLVMContext &ctx = getGlobalContext();
	BasicBlock *true_block = BasicBlock::Create(ctx, "true_block", cur_func);
	BasicBlock *false_block = BasicBlock::Create(ctx, "false_block", cur_func);
	BasicBlock *merge_block = BasicBlock::Create(ctx, "merge_block", cur_func);
	builder->CreateCondBr(cond, true_block, false_block);
	generateBlockCode(builder, true_block, merge_block, node->true_stmt);
	generateBlockCode(builder, false_block, merge_block, node->false_stmt);
	builder->SetInsertPoint(merge_block);
}

void LLVM::generateElseStmtCode(IRBuilder<> *builder, ElseStmtNode *node)
{
	generateCode(builder, node->stmt);
}

void LLVM::generateForStmtCode(IRBuilder<> *builder, ForStmtNode *node)
{
	LLVMContext &ctx = getGlobalContext();
	generateCode(builder, node->init);

	BasicBlock *loop_head = BasicBlock::Create(ctx, "loop_head", cur_func);
	BasicBlock *true_block = BasicBlock::Create(ctx, "true_block", cur_func);
	BasicBlock *after_block = BasicBlock::Create(ctx, "after_block", cur_func);
	builder->CreateBr(loop_head);
	builder->SetInsertPoint(loop_head);
	llvm::Value *_cond = generateValueCode(builder, node->cond);
	llvm::Value *zero = ConstantInt::get(IntegerType::get(module->getContext(), 64), 0);
	llvm::Value *cond = builder->CreateICmpNE(_cond, zero);
	builder->CreateCondBr(cond, true_block, after_block);

	builder->SetInsertPoint(true_block);
	Node *true_stmt = node->true_stmt;
	for (; true_stmt != NULL; true_stmt = true_stmt->next) {
		generateCode(builder, true_stmt);
	}
	generateCode(builder, node->progress);
	builder->CreateBr(loop_head);

	builder->SetInsertPoint(after_block);
}

void LLVM::generateForeachStmtCode(IRBuilder<> *builder, ForeachStmtNode *node)
{
	LLVMContext &ctx = getGlobalContext();
	BasicBlock *loop_head = BasicBlock::Create(ctx, "loop_head", cur_func);
	BasicBlock *true_block = BasicBlock::Create(ctx, "true_block", cur_func);
	BasicBlock *after_block = BasicBlock::Create(ctx, "after_block", cur_func);

	llvm::Value *array = generateValueCode(builder, node->cond);
	if (object_ptr_type->getTypeID() == array->getType()->getTypeID()) {
		//fprintf(stderr, "array wrapped by Object\n");
		llvm::Value *body = builder->CreateStructGEP(array, 1);
		llvm::Value *ovalue = builder->CreateLoad(builder->CreateStructGEP(body, 3));
		array = builder->CreatePointerCast(ovalue, array_ptr_type);
	}
	llvm::Value *array_size = builder->CreateLoad(builder->CreateStructGEP(array, 2));
	setIteratorValue(builder, node->itr);
	llvm::Value *itr_value = vmgr.getVariable(cur_func_name.c_str(), node->itr->tk->data.c_str(), node->itr->tk->finfo.indent)->value;
	llvm::Value *hidden_idx = builder->CreateAlloca(IntegerType::get(module->getContext(), 64), 0, "__hidden_idx__");
	llvm::Value *zero = ConstantInt::get(IntegerType::get(module->getContext(), 64), 0);
	llvm::Value *five = ConstantInt::get(IntegerType::get(module->getContext(), 64), 5);
	builder->CreateStore(zero, hidden_idx);

	builder->CreateBr(loop_head);
	builder->SetInsertPoint(loop_head);
	array_size = builder->CreatePtrToInt(array_size, IntegerType::get(module->getContext(), 64));
	llvm::Value *load_v = builder->CreateLoad(hidden_idx);
	llvm::Value *_cond = builder->CreateZExt(builder->CreateICmpSLT(load_v, array_size), llvm::Type::getInt64Ty(module->getContext()));
	llvm::Value *cond = builder->CreateICmpNE(_cond, zero);
	builder->CreateCondBr(cond, true_block, after_block);

	builder->SetInsertPoint(true_block);
	llvm::Value *elem = getArrayElement(builder, array, load_v);
	itr_value = elem;
	CodeGenerator::Value *itr = vmgr.getVariable(cur_func_name.c_str(), node->itr->tk->data.c_str(), node->itr->tk->finfo.indent);
	itr->value = elem;
	vmgr.setVariable(cur_func_name.c_str(), node->itr->tk->data.c_str(), node->itr->tk->finfo.indent, itr);

	Node *true_stmt = node->true_stmt;
	for (; true_stmt != NULL; true_stmt = true_stmt->next) {
		generateCode(builder, true_stmt);
	}

	llvm::Value *one = ConstantInt::get(IntegerType::get(module->getContext(), 64), 1);
	llvm::Value *incremented_value = builder->CreateAdd(builder->CreatePtrToInt(load_v, IntegerType::get(module->getContext(), 64)), one, "inc");
	builder->CreateStore(incremented_value, hidden_idx);
	builder->CreateBr(loop_head);

	builder->SetInsertPoint(after_block);
}

void LLVM::generateFunctionCode(IRBuilder<> *builder, FunctionNode *node)
{
	//llvm::FunctionType *ftype = llvm::FunctionType::get(object_type, false);
	FunctionType *ftype = llvm::FunctionType::get(IntegerType::get(module->getContext(), 64), array_ptr_type, false);
	Function *func = Function::Create(
		ftype,
		GlobalValue::ExternalLinkage,
		node->tk->data.c_str(), module
	);
	Function *main_func = func;
	cur_func = func;
	cur_func_name = node->tk->data.c_str();
	func->setCallingConv(CallingConv::C);
	BasicBlock *entry = BasicBlock::Create(module->getContext(), "entrypoint", func);
	builder->SetInsertPoint(entry);

	Node *body = node->body;
	for (; body != NULL; body = body->next) {
		generateCode(builder, body);
	}
	cur_func = main_func;
	builder->SetInsertPoint(main_entry);
}

void LLVM::generateReturnCode(IRBuilder<> *builder, ReturnNode *node)
{
	llvm::Value *ret = generateValueCode(builder, node->body);
	builder->CreateRet(ret);
}

void LLVM::setIteratorValue(IRBuilder<> *builder, Node *node)
{
	Token *tk = node->tk;
	llvm::Value *o = builder->CreateAlloca(object_type, 0, tk->data.c_str());
	CodeGenerator::Value *v = (CodeGenerator::Value *)malloc(sizeof(CodeGenerator::Value));
	v->type = cur_type;
	v->value = o;
	vmgr.setVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent, v);
}

llvm::Value *LLVM::getArrayElement(IRBuilder<> *builder, llvm::Value *array, llvm::Value *idx)
{
	llvm::Value *list = builder->CreateLoad(builder->CreateStructGEP(array, 1));
	return builder->CreateLoad(builder->CreateGEP(list, idx));
}

void LLVM::generateWhileStmtCode(IRBuilder<> *builder, WhileStmtNode *node)
{
	LLVMContext &ctx = getGlobalContext();

	BasicBlock *loop_head = BasicBlock::Create(ctx, "loop_head", cur_func);
	BasicBlock *true_block = BasicBlock::Create(ctx, "true_block", cur_func);
	BasicBlock *after_block = BasicBlock::Create(ctx, "after_block", cur_func);
	builder->CreateBr(loop_head);
	builder->SetInsertPoint(loop_head);
	llvm::Value *_cond = generateValueCode(builder, node->expr);
	llvm::Value *zero = ConstantInt::get(IntegerType::get(module->getContext(), 64), 0);
	llvm::Value *cond = builder->CreateICmpNE(_cond, zero);
	builder->CreateCondBr(cond, true_block, after_block);

	builder->SetInsertPoint(true_block);
	Node *true_stmt = node->true_stmt;
	for (; true_stmt != NULL; true_stmt = true_stmt->next) {
		generateCode(builder, true_stmt);
	}
	builder->CreateBr(loop_head);

	builder->SetInsertPoint(after_block);
}

void LLVM::generateSingleTermOperatorCode(IRBuilder<> *builder, SingleTermOperatorNode *node)
{
	using namespace TokenType;
	llvm::Value *value = generateValueCode(builder, node->expr);
	Token *tk = node->expr->tk;
	Enum::Runtime::Type type = cur_type;
	switch (node->tk->info.type) {
	case Inc:
		if (type == Enum::Runtime::Int) {
			CodeGenerator::Value *v = vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent);
			llvm::Value *one = ConstantInt::get(IntegerType::get(module->getContext(), 64), 1);
			setLLVMValue(builder, v->value, type, builder->CreateAdd(value, one, "inc"));
			cur_type = Enum::Runtime::Int;
		} else {
			llvm::Value *casted_value = builder->CreateSIToFP(value, llvm::Type::getDoubleTy(module->getContext()));
			CodeGenerator::Value *v = vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent);
			llvm::Value *one = ConstantFP::get(llvm::Type::getDoubleTy(module->getContext()), 1.0);
			setLLVMValue(builder, v->value, type, builder->CreateFAdd(casted_value, one, "inc"));
			cur_type = Enum::Runtime::Double;
		}
		break;
	case Dec:
		if (type == Enum::Runtime::Int) {
			CodeGenerator::Value *v = vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent);
			llvm::Value *one = ConstantInt::get(IntegerType::get(module->getContext(), 64), 1);
			setLLVMValue(builder, v->value, type, builder->CreateSub(value, one, "dec"));
			cur_type = Enum::Runtime::Int;
		} else {
			llvm::Value *casted_value = builder->CreateSIToFP(value, llvm::Type::getDoubleTy(module->getContext()));
			CodeGenerator::Value *v = vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent);
			llvm::Value *one = ConstantFP::get(llvm::Type::getDoubleTy(module->getContext()), 1.0);
			setLLVMValue(builder, v->value, type, builder->CreateFSub(casted_value, one, "dec"));
			cur_type = Enum::Runtime::Double;
		}
		break;
	default:
		break;
	}
}

llvm::Value *LLVM::generateAssignCode(IRBuilder<> *builder, BranchNode *node)
{
	llvm::Value *value = generateValueCode(builder, node->right);
	//fprintf(stderr, "AssignSection: value type = ");
	//value->getType()->dump();
	//fprintf(stderr, "\n");
	Token *tk = node->left->tk;
	CodeGenerator::Value *v = vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent);
	if (!v) {
		llvm::Value *tmp = builder->CreateAlloca(object_type, 0, node->left->tk->data.c_str());
		llvm::Value *o = setLLVMValue(builder, tmp, cur_type, value);
		v = (CodeGenerator::Value *)malloc(sizeof(CodeGenerator::Value));
		v->type = cur_type;
		v->value = o;
		//fprintf(stderr, "SET object pointer = [%p]\n", o);
		vmgr.setVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent, v);
	} else {
		//fprintf(stderr, "AssignSection: v->value type = ");
		//v->value->getType()->dump();
		//fprintf(stderr, "\n");
		setLLVMValue(builder, v->value, cur_type, value);
	}
	//builder->CreateStore(o, var);
	return v->value;
	//return var;
}

llvm::Value *LLVM::generateOperatorCode(IRBuilder<> *builder, BranchNode *node)
{
	using namespace TokenType;
	llvm::Value *left_value = generateValueCode(builder, node->left);
	Enum::Runtime::Type left_type = cur_type;
	llvm::Value *right_value = generateValueCode(builder, node->right);
	Enum::Runtime::Type right_type = cur_type;
	llvm::Value *ret = NULL;
	//generateCastCode(builder, left_value);
	switch (node->tk->info.type) {
	case Add:
		SET_PRIMITIVE_OPCODE(builder->CreateAdd, builder->CreateFAdd, "add", ret);
		break;
	case Sub:
		SET_PRIMITIVE_OPCODE(builder->CreateSub, builder->CreateFSub, "sub", ret);
		break;
	case Mul:
		SET_PRIMITIVE_OPCODE(builder->CreateMul, builder->CreateFMul, "mul", ret);
		break;
	case Div:
		SET_PRIMITIVE_OPCODE(builder->CreateExactSDiv, builder->CreateFDiv, "div", ret);
		break;
	case BitAnd:
		SET_PRIMITIVE_OPCODE(builder->CreateAnd, builder->CreateAnd, "&", ret);
		break;
	case Mod:
		break;
	case EqualEqual:
		SET_COMPARE_OPCODE(builder->CreateICmpEQ, builder->CreateFCmpOEQ, "eq", ret);
		break;
	case NotEqual:
		SET_COMPARE_OPCODE(builder->CreateICmpNE, builder->CreateFCmpONE, "ne", ret);
		break;
	case Greater:
		SET_COMPARE_OPCODE(builder->CreateICmpSGT, builder->CreateFCmpOGT, "gt", ret);
		break;
	case Less:
		SET_COMPARE_OPCODE(builder->CreateICmpSLT, builder->CreateFCmpOLT, "lt", ret);
		break;
	default:
		break;
	}
	return ret;
}

llvm::Value *LLVM::generateListCode(IRBuilder<> *builder, ListNode *node)
{
	Node *data = node->data;
	vector<CodeGenerator::Value *> list;
	if (data->tk->info.type == TokenType::Comma) {
		generateCommaCode(builder, dynamic_cast<BranchNode *>(data), &list);
	} else {
		CodeGenerator::Value *v = new CodeGenerator::Value();
		v->value = generateValueCode(builder, data);
		v->type = cur_type;
		v->tk = data->tk;
		list.push_back(v);
	}
	size_t size = list.size();
	llvm::Value *elems = builder->CreateAlloca(object_ptr_type, ConstantInt::get(IntegerType::get(module->getContext(), 64), size), "elems");
	for (size_t i = 0; i < size; i++) {
		CodeGenerator::Value *v = list.at(i);
		llvm::Value *value = v->value;
		Enum::Runtime::Type type = v->type;
		llvm::Value *elem = builder->CreateAlloca(object_type, 0, "elem");
		llvm::Value *idx = ConstantInt::get(IntegerType::get(module->getContext(), 64), i);
		if (type == Enum::Runtime::Object) {
			Token *tk = v->tk;
			builder->CreateStore(vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent)->value, builder->CreateGEP(elems, idx));
		} else {
			builder->CreateStore(setLLVMValue(builder, elem, type, value), builder->CreateGEP(elems, idx));
		}
	}
	cur_type = Enum::Runtime::Array;
	return makeArgumentArray(builder, elems, size);
}

void LLVM::generateCommaCode(IRBuilder<> *builder, BranchNode *node, vector<CodeGenerator::Value *> *list)
{
	BranchNode *branch = dynamic_cast<BranchNode *>(node);
	Node *left = branch->left;
	if (left->tk->info.type == TokenType::Comma) {
		generateCommaCode(builder, dynamic_cast<BranchNode *>(branch->left), list);
	} else {
		CodeGenerator::Value *value = new CodeGenerator::Value();
		value->value = generateValueCode(builder, branch->left);
		value->type = cur_type;
		value->tk = branch->left->tk;
		list->push_back(value);
	}
	if (!branch->right) return;
	Node *right = branch->right;
	if (right->tk->info.type == TokenType::Comma) {
		generateCommaCode(builder, dynamic_cast<BranchNode *>(branch->right), list);
	} else {
		CodeGenerator::Value *value = new CodeGenerator::Value();
		value->value = generateValueCode(builder, branch->right);
		value->type = cur_type;
		value->tk = branch->right->tk;
		list->push_back(value);
	}
}

llvm::Value *LLVM::generateArrayAccessCode(IRBuilder<> *builder, ArrayNode *node)
{
	llvm::Value *ret = NULL;
	llvm::Value *idx = generateValueCode(builder, node->idx);
	if (node->tk->type == TokenType::SpecificValue) {
		Function::ArgumentListType &args = cur_func->getArgumentList();
		Function::ArgumentListType::iterator it = args.begin();
		Argument *arg = &*it;
		ret = getArrayElement(builder, arg, idx);
	}
	cur_type = Enum::Runtime::Object;
	return ret;
}

llvm::Value *LLVM::generateValueCode(IRBuilder<> *builder, Node *node)
{
	using namespace TokenType;
	llvm::Value *ret = NULL;
	Token *tk = node->tk;
	switch (tk->info.kind) {
	case TokenKind::Operator:
		ret = generateOperatorCode(builder, dynamic_cast<BranchNode *>(node));
		break;
	default:
		break;
	}
	if (TYPE_match(node, ListNode)) {
		ret = generateListCode(builder, dynamic_cast<ListNode *>(node));
	} else if (TYPE_match(node, FunctionCallNode)) {
		ret = generateFunctionCallCode(builder, dynamic_cast<FunctionCallNode *>(node));
	} else if (TYPE_match(node, ArrayNode)) {
		ret = generateArrayAccessCode(builder, dynamic_cast<ArrayNode *>(node));
	}
	if (ret) return ret;
	switch (tk->info.type) {
	case String:
		ret = builder->CreateGlobalStringPtr(tk->data.c_str());
		cur_type = Enum::Runtime::String;
		break;
	case RawString:
		break;
	case ExecString:
		break;
	case Int:
		ret = ConstantInt::get(IntegerType::get(module->getContext(), 64), atoi(tk->data.c_str()));
		cur_type = Enum::Runtime::Int;
		break;
	case Double:
		ret = ConstantFP::get(llvm::Type::getDoubleTy(module->getContext()), atof(tk->data.c_str()));
		cur_type = Enum::Runtime::Double;
		break;
	case ArrayVar: case Var: {
		ValueSymbolTable &vs_table = cur_func->getValueSymbolTable();
		CodeGenerator::Value *var = vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent);
		//fprintf(stderr, "GET object pointer = [%p]\n", var->value);
		llvm::Value *value = builder->CreateStructGEP(var->value, 1);
		cur_type = var->type;
		switch (var->type) {
		case Enum::Runtime::Int:
			ret = builder->CreateLoad(builder->CreateStructGEP(value, 0));
			break;
		case Enum::Runtime::Double:
			ret = builder->CreateLoad(builder->CreateStructGEP(value, 1));
			break;
		case Enum::Runtime::String:
			ret = builder->CreateLoad(builder->CreateStructGEP(value, 2));
			break;
		default:
			ret = var->value;
			//fprintf(stderr, "ValueSection: var->value->type = ");
			//var->value->getType()->dump();
			//fprintf(stderr, "\n");
			cur_type = Enum::Runtime::Object;
			break;
		}
		break;
	}
	default:
		break;
	}
	return ret;
}

llvm::Value *LLVM::generateFunctionCallCode(IRBuilder<> *builder, FunctionCallNode *node)
{
	vector<llvm::Value *> values;
	vector<Enum::Runtime::Type> types;
	vector<Token *> tokens;
	llvm::Value *vargs;
	if (node->args) {
		size_t size = node->args->size();
		for (size_t i = 0; i < size; i++) {
			Node *arg = node->args->at(i);
			ListNode *list_node = dynamic_cast<ListNode* >(arg);
			Node *data = (list_node) ? list_node->data : arg;
			if (data->tk->info.type == TokenType::Comma) {
				BranchNode *branch = dynamic_cast<BranchNode *>(data);
				values.push_back(generateValueCode(builder, branch->left));
				types.push_back(cur_type);
				tokens.push_back(branch->left->tk);
				if (branch->right) {
					values.push_back(generateValueCode(builder, branch->right));
					types.push_back(cur_type);
					tokens.push_back(branch->right->tk);
				}
			} else {
				values.push_back(generateValueCode(builder, data));
				types.push_back(cur_type);
				tokens.push_back(data->tk);
			}
		}
		size_t args_num = values.size();
		llvm::Value *__args__ = builder->CreateAlloca(object_ptr_type, ConstantInt::get(IntegerType::get(module->getContext(), 64), args_num), "__args__");
		for (size_t i = 0; i < args_num; i++) {
			llvm::Value *value = values.at(i);
			Enum::Runtime::Type type = types.at(i);
			//fprintf(stderr, "type = [%d]\n", type);
			llvm::Value *arg = builder->CreateAlloca(object_type, 0, "__arg__");
			llvm::Value *idx = ConstantInt::get(IntegerType::get(module->getContext(), 64), i);
			if (type == Enum::Runtime::Object || type == Enum::Runtime::Array) {
				Token *tk = tokens.at(i);
				builder->CreateStore(vmgr.getVariable(cur_func_name.c_str(), tk->data.c_str(), tk->finfo.indent)->value, builder->CreateGEP(__args__, idx));
			} else {
				builder->CreateStore(setLLVMValue(builder, arg, type, value), builder->CreateGEP(__args__, idx));
			}
		}
		vargs = makeArgumentArray(builder, __args__, args_num);
	}
	llvm::Value *ret = NULL;
	cur_args = vargs;
	if (node->tk->type == TokenType::BuiltinFunc) {
		string name = node->tk->data;
		Constant *func = getBuiltinFunction(builder, name);
		//fprintf(stderr, "created function\n");
		//builder->CreateCall(func);
		//module->dump();
		ret = builder->CreateCall(func, vargs);
	} else {
		//getFunction(node->tk->data.c_str());
		ret = builder->CreateCall(cur_func, vargs);
	}
	cur_type = Enum::Runtime::Int;
	return ret;
}

Constant *LLVM::getBuiltinFunction(IRBuilder<> *builder, string name)
{
	Constant *ret = NULL;
	vector<Type*> arg_types;
	if (name == "print" || name == "say") {
		FunctionType *ftype = llvm::FunctionType::get(builder->getVoidTy(), array_ptr_type, false);
		//ftype->getParamType(0)->dump();
		ret = module->getOrInsertFunction(name, ftype);
	} else if (name == "puts") {
		arg_types.push_back(builder->getInt8Ty()->getPointerTo());
		ArrayRef<Type*> arg_types_ref(arg_types);
		FunctionType* ftype = FunctionType::get(
			IntegerType::get(module->getContext(), 32),
			arg_types_ref,
			false);
		ret = module->getOrInsertFunction(name, ftype);
	} else {
		//fprintf(stderr, "create runtime function\n");
		FunctionType *ftype = llvm::FunctionType::get(builder->getVoidTy(), array_ptr_type, false);
		//ftype->getParamType(0)->dump();
		ret = Function::Create(ftype, Function::ExternalLinkage, "print", module);
		//ret = module->getOrInsertFunction("runtime", ftype);
	}
	return ret;
}

llvm::Value *LLVM::setLLVMValue(IRBuilder<> *builder, llvm::Value *runtime_object, Enum::Runtime::Type type, llvm::Value *value)
{
	LLVMContext &ctx = getGlobalContext();
	llvm::Value *obj_type = builder->CreateStructGEP(runtime_object, 0);
	llvm::Value *obj_value = builder->CreateStructGEP(runtime_object, 1);
	switch (type) {
	case Enum::Runtime::Int:
		builder->CreateStore(value, builder->CreateStructGEP(obj_value, 0));
		break;
	case Enum::Runtime::Double:
		builder->CreateStore(value, builder->CreateStructGEP(obj_value, 1));
		break;
	case Enum::Runtime::String:
		builder->CreateStore(value, builder->CreateStructGEP(obj_value, 2));
		break;
	default: {
		Type *int_ptr_type = Type::getInt8PtrTy(ctx);
		llvm::Value *ptr = builder->CreatePointerCast(value, int_ptr_type);
		builder->CreateStore(ptr, builder->CreateStructGEP(obj_value, 3));
		break;
	}
	}
	builder->CreateStore(ConstantInt::get(IntegerType::get(module->getContext(), 32), (int)type), obj_type);
	return runtime_object;
}

llvm::Value *LLVM::makeArgumentArray(IRBuilder<> *builder, llvm::Value *list, size_t size)
{
	llvm::Value *args = builder->CreateAlloca(array_type, 0, "arg_array");
	llvm::Value *array_type = builder->CreateStructGEP(args, 0);
	llvm::Value *array_list = builder->CreateStructGEP(args, 1);
	llvm::Value *array_size = builder->CreateStructGEP(args, 2);
	builder->CreateStore(ConstantInt::get(IntegerType::get(module->getContext(), 32), (int)Enum::Runtime::Array), array_type);
	builder->CreateStore(list, array_list);
	builder->CreateStore(ConstantInt::get(IntegerType::get(module->getContext(), 64), size), array_size);
	return args;
}

void LLVM::debug_run(AST *ast)
{
	gen(ast);
	string errStr;
	InitializeNativeTarget();
	ExecutionEngine *engine = EngineBuilder(module).setErrorStr(&errStr).setUseMCJIT(true).create();
	//cout << errStr << endl;
	Function *main = NULL;
	vector<GenericValue> args;
	assert((main = module->getFunction("main")) && "main function was not declared");
	engine->runFunction(main, args);
	//fprintf(stderr, "%d\n", program());
}
